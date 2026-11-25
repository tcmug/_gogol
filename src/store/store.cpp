// store.cpp — implementation. Mirrors experiments/mem-v2/memstore.py.
#include "store/store.h"
#include <cstdlib>

#include "storage/index_file.h" // float_to_f16 / f16_to_float
#include "cli/git_identity.h"   // resolve_root_sha (notes scope_id backfill)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <ctime>
#include <set>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "monocypher.h"
}

namespace store {

// cache.db indexer schema version. cache.db is DERIVED / disposable — when this
// bumps, an older cache.db (whose columns/tables predate the current schema) is
// DROPPED and rebuilt on open (a re-index repopulates it; no data migration).
// Bump this whenever kSchema's table/column shape changes.
static const uint32_t kCacheSchemaVersion = 5;  // v5: ref_sightings location layer

// cache.db schema. The file is cache.db and holds DERIVED / re-derivable data
// (code+doc entries + embeddings, the graph, metrics, FTS) — so every table is
// `cache_`-prefixed to say exactly that. A cache rebuild only ever touches this
// file; notes (content of record) live in notes.db (kNotesSchema).
static const char *kSchema =
    "CREATE TABLE IF NOT EXISTS entries ("
    "  id TEXT PRIMARY KEY, content TEXT NOT NULL, embedding BLOB,"
    "  source TEXT, idx TEXT,"
    // root_sha: the repo's root commit SHA (content-derived, location-independent
    // identity). Set at index time alongside idx (the path/common_dir). Lets a
    // container scope by a SHA token it computed from its OWN checkout, with no
    // shared filesystem (idx is a host path the container can't name). NULL when the
    // repo's root is ambiguous/empty or for legacy rows (backfilled on next resync).
    "  root_sha TEXT,"
    "  line INTEGER, end_line INTEGER, symbol TEXT,"
    // Per-function complexity metrics INLINED onto the chunk row (Step 5). Computed at
    // chunk time by exact line match (the function whose decl line falls in this
    // chunk's range) — NOT a fuzzy post-join (which mis-attributed ~46% of the time).
    // NULL for a chunk that holds no function (e.g. a doc/markdown chunk).
    "  complexity INTEGER, lines INTEGER, params INTEGER, returns INTEGER, max_depth INTEGER);"
    // Membership is 1:1 under the git-path model — an entry belongs to exactly ONE
    // repo (the `idx` column = index/repo name), so scoping is WHERE idx=? (no
    // junction table). Index it for fast per-repo scans.
    "CREATE INDEX IF NOT EXISTS entries_idx ON entries(idx);"
    // Index entries.source: GC (purge_missing_sources) and the per-file delete surfaces
    // key on it (WHERE source=?); without this they full-scan entries.
    "CREATE INDEX IF NOT EXISTS entries_source ON entries(source) WHERE source IS NOT NULL;"
    // NB: the entries_root_sha index is created in the v4 migration block (NOT here) —
    // on a legacy table the column is added by ALTER first, so indexing it in this
    // static schema would fail (no such column) before the ALTER runs.
    // entry_sources: tracks which toplevels (checkouts) an entry appears in.
    // Same content in main + worktree = one entry row, two entry_sources rows.
    // Query joins on toplevel to scope results to a specific checkout.
    // PK includes source (v3): two identical-content files within ONE checkout
    // get separate rows — the old (entry_id, toplevel) PK silently overwrote the
    // first file's path, which would make a GC of one file destroy the shared
    // entry for the other.
    "CREATE TABLE IF NOT EXISTS entry_sources ("
    "  entry_id TEXT NOT NULL, toplevel TEXT NOT NULL, source TEXT NOT NULL,"
    "  PRIMARY KEY(entry_id, toplevel, source),"
    "  FOREIGN KEY(entry_id) REFERENCES entries(id) ON DELETE CASCADE);"
    "CREATE INDEX IF NOT EXISTS entry_sources_toplevel ON entry_sources(toplevel);"
    // Index entry_sources.source: the PK is (entry_id, toplevel, source), so source is
    // NOT the leftmost key — a WHERE source=? (GC, migration) can't use the PK. Index it.
    "CREATE INDEX IF NOT EXISTS entry_sources_source ON entry_sources(source);"
    // Per-file stat-hash (mtime+size), keyed by absolute source path. Lets sync skip
    // re-chunking/re-embedding files whose stat hasn't changed since last sync —
    // the incremental-sync fast path. One row per file (cheap).
    // Per-file stat-hash for the incremental-sync skip gate. Keyed by (idx, source):
    // source is repo-RELATIVE and thus not unique across repos, so idx is part of the
    // key (two repos can both have "src/a.ts" with different hashes).
    "CREATE TABLE IF NOT EXISTS file_hashes ("
    "  idx TEXT NOT NULL DEFAULT '', source TEXT NOT NULL, hash INTEGER NOT NULL,"
    "  PRIMARY KEY(idx, source));"
    "CREATE TABLE IF NOT EXISTS edges ("
    "  kind TEXT NOT NULL, src_sym TEXT NOT NULL, dst_sym TEXT NOT NULL, idx TEXT,"
    "  file TEXT, PRIMARY KEY(kind, src_sym, dst_sym, idx));"
    "CREATE INDEX IF NOT EXISTS edges_src ON edges(src_sym);"
    // Index edges.file: GC + remove_edges_of_file key on it (WHERE file=?).
    "CREATE INDEX IF NOT EXISTS edges_file ON edges(file);"
    "CREATE INDEX IF NOT EXISTS edges_dst ON edges(dst_sym);"
    // symbol->id resolution is the entries.symbol column (Step 4: the separate
    // symbols map table was collapsed into it — entry deletion removes the
    // symbol mapping inherently, no orphan cleanup). Indexed for delve/links-by-name.
    "CREATE INDEX IF NOT EXISTS entries_symbol ON entries(symbol);"
    // Rank refactor candidates by inlined complexity (Step 5). Partial index skips the
    // NULL (non-function) rows so top_complexity only scans real functions.
    "CREATE INDEX IF NOT EXISTS entries_cx ON entries(complexity) WHERE complexity IS NOT NULL;"
    // FTS5 keyword arm (Step 7). External-content index over entries(content,
    // symbol) — the keyword half of the hybrid recall (blended with cosine; see
    // query_hybrid's A3 weighted blend).
    // Kept in sync by triggers so it never drifts from entries. `symbol` is the
    // function/heading name; content carries the signature text, so a name/signature
    // keyword match is covered.
    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5("
    "  content, symbol, content='entries', content_rowid='rowid');"
    "CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN"
    "  INSERT INTO entries_fts(rowid, content, symbol)"
    "  VALUES (new.rowid, new.content, new.symbol); END;"
    "CREATE TRIGGER IF NOT EXISTS entries_ad AFTER DELETE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, content, symbol)"
    "  VALUES ('delete', old.rowid, old.content, old.symbol); END;"
    "CREATE TRIGGER IF NOT EXISTS entries_au AFTER UPDATE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, content, symbol)"
    "  VALUES ('delete', old.rowid, old.content, old.symbol);"
    "  INSERT INTO entries_fts(rowid, content, symbol)"
    "  VALUES (new.rowid, new.content, new.symbol); END;"
    // vec0 ANN index for fast KNN query (replaces brute-force cosine).
    // Keyed by entries.id (TEXT), stores f32 vectors for cosine distance.
    // sqlite-vec handles the index structure; we just INSERT/DELETE alongside entries.
    // INVARIANT §1 (docs/INVARIANTS.md): NO ON DELETE CASCADE — reconcile by hand in
    // every entry-delete path (remove_by_source / remove_stale_chunks).
    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_vec USING vec0("
    "  id TEXT PRIMARY KEY, embedding float[768] distance_metric=cosine);"
    // ref_sightings: the LOCATION layer (helper, not identity). Maps a repo's identity
    // (root_sha, the load-bearing idx) to on-disk paths it has been seen at, with a
    // last_seen stamp and an overlay flag. Derived/rebuildable: wiping it costs only
    // re-learning on the next invocation. Powers parent-dir multi-repo search, move
    // detection, display paths, and user overlays — all verified on use by fs::exists.
    "CREATE TABLE IF NOT EXISTS ref_sightings ("
    "  root_sha TEXT NOT NULL, path TEXT NOT NULL, last_seen INTEGER,"
    "  overlay INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(root_sha, path));"
    "CREATE INDEX IF NOT EXISTS ref_sightings_path ON ref_sightings(path);"
    "CREATE INDEX IF NOT EXISTS ref_sightings_overlay ON ref_sightings(overlay) WHERE overlay=1;";

// notes.db schema — the note CONTENT of record. Deliberately tiny: a note is its
// INVARIANT §2 (docs/INVARIANTS.md): notes are a SEPARATE world — no entry_sources,
// no edges, no FTS. Scoped by scope_path (path-prefix), NOT by toplevel visibility,
// so recall must bypass the entry_sources join for note ids (bug 444ad8b). notes_vec
// has the same no-cascade rule as entries_vec (§1).
// content-hash id, the content, its embedding (co-located so a cache rebuild leaves
// notes fully queryable), an optional scope_path (path-prefix surfacing), and a
// created stamp. No graph/FTS/membership here — notes.db holds ONLY notes, so no
// bulk/rebuild surface can touch anything of record.
static const char *kNotesSchema =
    "CREATE TABLE IF NOT EXISTS notes ("
    "  id TEXT PRIMARY KEY, content TEXT NOT NULL, embedding BLOB,"
    // Note scope (three-shape model, see note_scope_matches):
    //   scope_id = repo ROOT SHA (portable identity) when the note belongs to a
    //     single-root repo; scope_path then holds the subpath RELATIVE to the repo root
    //     (NULL = whole repo). Survives clone/move.
    //   scope_id NULL + scope_path ABSOLUTE = a note on a non-repo / multi-root dir
    //     (matched by absolute path-prefix; not portable).
    //   both NULL = a GLOBAL note (always surfaces).
    // So scope_path's meaning (relative vs absolute) is conditional on scope_id.
    "  scope_path TEXT, scope_id TEXT, created INTEGER, supersedes TEXT);"
    "CREATE INDEX IF NOT EXISTS notes_scope ON notes(scope_path);"
    "CREATE INDEX IF NOT EXISTS notes_scope_id ON notes(scope_id) WHERE scope_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS notes_supersedes ON notes(supersedes) WHERE supersedes IS NOT NULL;"
    // vec0 ANN index for notes (same pattern as entries_vec).
    "CREATE VIRTUAL TABLE IF NOT EXISTS notes_vec USING vec0("
    "  id TEXT PRIMARY KEY, embedding float[768] distance_metric=cosine);";

std::string content_id(const std::string &content) {
    // INVARIANT §1 (docs/INVARIANTS.md): id = BLAKE2b(content) alone. Changing content
    // yields a NEW id — old rows orphan, so every satellite table (entries_vec, edges,
    // file_hashes) must be reconciled on change/delete. See remove_by_source /
    // remove_stale_chunks.
    // Hash layout preserved for id stability: <NUL> + content (formerly salt+NUL+content
    // with salt always empty). Identity is pure content hash — dedup is the invariant.
    std::string buf;
    buf.push_back('\0');
    buf += content;
    uint8_t hash[32];
    crypto_blake2b(hash, sizeof(hash),
                   reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 8; i++) { // 8 bytes -> 16 hex chars
        out.push_back(hex[hash[i] >> 4]);
        out.push_back(hex[hash[i] & 0xF]);
    }
    return out;
}

static void l2_normalize(std::vector<float> &v) {
    double n = 0;
    for (float x : v) n += double(x) * x;
    n = std::sqrt(n) + 1e-9;
    for (float &x : v) x = float(x / n);
}

// Build a safe FTS5 MATCH expression from free text. Each whitespace-separated term
// is wrapped in double quotes (a bare string literal) so FTS5 operators/special chars
// (AND/OR/NOT/NEAR, *, ^, :, parens, hyphens) can't break the MATCH; embedded quotes
// are doubled per FTS5 rules. Terms are OR-joined (any term may match). 1-char terms
// dropped. Returns "" when nothing usable remains (caller then skips the FTS arm).
// Mirrors SqliteBackend::build_fts_match (the daemon keyword engine) 1:1.
static std::string build_fts_match(const std::string &query) {
    std::string out;
    size_t i = 0, n = query.size();
    while (i < n) {
        while (i < n && std::isspace((unsigned char)query[i])) i++;
        size_t start = i;
        while (i < n && !std::isspace((unsigned char)query[i])) i++;
        if (i <= start) continue;
        std::string term = query.substr(start, i - start);
        if (term.size() < 2) continue;
        std::string quoted = "\"";
        for (char c : term) { if (c == '"') quoted += "\"\""; else quoted += c; }
        quoted += "\"";
        if (!out.empty()) out += " OR ";
        out += quoted;
    }
    return out;
}

// Ensure the store's schema exists/is current, using a transient read-write
// connection that closes immediately. Called before a read-only open so readers
// never find missing tables (first run / schema upgrade) — yet the read connection
// itself stays read-only (lock-free). If the schema is already current this still
// briefly opens RW, but the migration body is cheap (CREATE IF NOT EXISTS + a couple
// of column checks); it does NOT re-embed or scan.
static bool schema_ready(const std::string &path) {
    // Cheap read-only probe: is the file already at the current schema version? If
    // not (older/absent), a transient RW open will drop+rebuild it. Avoids a
    // write-lock open in the common (already-current) case.
    try {
        Db probe(path, /*read_only=*/true);
        return probe.user_version() == kCacheSchemaVersion;
    } catch (...) {
        return false; // can't even open read-only (no file yet) -> not ready
    }
}

// Ensure schema is current via a transient read-write connection (closes at once),
// then the caller opens read-only. Only does real DDL work when NOT already current
// (first run / upgrade); otherwise it's a cheap CREATE-IF-NOT-EXISTS pass. Returns
// path so it can be chained in a member-init list.
static const std::string &ensure_schema_ro(const std::string &path) {
    if (!schema_ready(path)) {
        // One-time migration: open RW cache-only (explicit empty notes path, so no
        // notes.db sibling is created here), run the same cache schema/migration the RW
        // ctor does. Scoped so the RW connection closes before the read-only one opens.
        Store migrate(path, /*notes_path=*/std::string());
        (void)migrate;
    }
    return path;
}

// notes.db equivalent: ensure the `notes` table exists via a transient RW
// connection (closes at once), then the caller opens read-only. Cheap CREATE-IF-
// NOT-EXISTS in the already-migrated case.
static bool notes_schema_ready(const std::string &path) {
    try {
        Db probe(path, /*read_only=*/true);
        Db::Stmt q(probe, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='notes'");
        if (!q.step()) return false;
        // A legacy notes.db may already have the table but be missing the columns the
        // current note model relies on. Require both the scope_id and supersedes columns
        // before the reader opens; `table_info(notes)` col 1 = column name.
        bool has_scope_id = false, has_supersedes = false;
        Db::Stmt ti(probe, "PRAGMA table_info(notes)");
        while (ti.step()) {
            std::string column = ti.column_text(1);
            if (column == "scope_id") has_scope_id = true;
            if (column == "supersedes") has_supersedes = true;
        }
        return has_scope_id && has_supersedes;
    } catch (...) {
        return false; // no file yet -> not ready
    }
}

static const std::string &ensure_notes_schema_ro(const std::string &path) {
    if (!notes_schema_ready(path)) {
        Db migrate(path);          // RW, creates the file + notes schema
        // ORDER MATTERS: if a legacy `notes` table already exists, add any missing
        // columns BEFORE exec(kNotesSchema). kNotesSchema's `CREATE INDEX ... ON
        // notes(supersedes)` references the column, so running the schema first throws
        // `no such column: supersedes` on a pre-supersedes db — before the ALTER below
        // ever runs (bug: heal silently failed, surfaced as "busy or unavailable").
        // The ALTERs are no-ops when the table doesn't exist yet (PRAGMA returns no
        // rows), and kNotesSchema then creates everything fresh; when it does exist,
        // the ALTERs fill the gaps so the subsequent index creation succeeds.
        bool table_exists = false, has_scope_id = false, has_supersedes = false;
        {
            Db::Stmt ti(migrate, "PRAGMA table_info(notes)");
            while (ti.step()) {
                table_exists = true;
                std::string column = ti.column_text(1);
                if (column == "scope_id") has_scope_id = true;
                if (column == "supersedes") has_supersedes = true;
            }
        }
        if (table_exists) {
            if (!has_scope_id)  migrate.exec("ALTER TABLE notes ADD COLUMN scope_id TEXT");
            if (!has_supersedes) migrate.exec("ALTER TABLE notes ADD COLUMN supersedes TEXT");
        }
        migrate.exec(kNotesSchema);   // now safe: columns the indexes reference all exist
    }
    return path;
}

// Derive the notes.db path when a caller opens a store with only a cache path.
// Real callers (CLI/MCP) pass both explicitly; tests and internal one-time opens
// pass only the cache path — deriving a sibling keeps those stores fully functional
// (notes round-trip) instead of silently no-op'ing note writes. Rule: strip a
// trailing ".db" and append ".notes.db" (so "x.db" -> "x.notes.db", "x" -> "x.notes.db").
static std::string derive_notes_path(const std::string &cache_path) {
    if (cache_path.empty()) return "";
    std::string base = cache_path;
    const std::string dbext = ".db";
    if (base.size() >= dbext.size() &&
        base.compare(base.size() - dbext.size(), dbext.size(), dbext) == 0)
        base = base.substr(0, base.size() - dbext.size());
    return base + ".notes.db";
}

Store::Store(const std::string &path, ReadOnly) : Store(path, derive_notes_path(path), ReadOnly{}) {}

Store::Store(const std::string &path, const std::string &notes_path, ReadOnly)
    : db_(ensure_schema_ro(path), /*read_only=*/true), db_path_(path) {
    // INVARIANT §7 (docs/INVARIANTS.md): reads open read-only so they never take a
    // write lock and contend with a concurrent sync (bug b8599aa).
    // Reads must not take a write lock (so they don't contend with a sync). But a
    // store not yet at the current schema (first run / upgrade) has no tables to read.
    // So ensure_schema_ro does a ONE-TIME read-write migration iff needed, then this
    // opens PURE read-only. Steady state = lock-free reads; migration paid once.
    if (!notes_path.empty()) {
        // Same pattern for notes.db: ensure the schema exists via a transient RW
        // connection, then open the reader read-only (lock-free). A note query never
        // contends with a note write.
        notes_db_ = std::make_unique<Db>(ensure_notes_schema_ro(notes_path), /*read_only=*/true);
    }
}

Store::Store(const std::string &path) : Store(path, derive_notes_path(path)) {}

Store::Store(const std::string &path, const std::string &notes_path) : db_(path), db_path_(path) {
    // cache.db is DERIVED / disposable — a re-index rebuilds it from disk. There is no
    // deployed store to migrate (this is unreleased), so the schema is simply created at
    // version 1. PRAGMA user_version stamps it (a lightweight header int, no meta table);
    // if the schema ever changes in a way that matters, bump kCacheSchemaVersion and add
    // a rebuild then — not before it's needed.
    db_.exec(kSchema);
    db_.set_user_version(kCacheSchemaVersion);

    // v3 migration: entry_sources PK widened (entry_id, toplevel) ->
    // (entry_id, toplevel, source). kSchema's CREATE IF NOT EXISTS can't alter an
    // existing table, so rebuild it in place when the old PK is detected. Rows are
    // copied as-is — dropping the table wholesale would desync stat-hash-skipped
    // files until their next content change.
    {
        bool needs_migration = false;
        {
            Db::Stmt q(db_, "SELECT sql FROM sqlite_master WHERE type='table' AND name='entry_sources'");
            std::string sql = q.step() ? q.column_text(0) : std::string();
            needs_migration = !sql.empty() &&
                sql.find("PRIMARY KEY(entry_id, toplevel, source)") == std::string::npos;
        }
        if (needs_migration) {
            Db::Tx tx(db_);
            db_.exec("CREATE TABLE entry_sources_v3 ("
                     "  entry_id TEXT NOT NULL, toplevel TEXT NOT NULL, source TEXT NOT NULL,"
                     "  PRIMARY KEY(entry_id, toplevel, source),"
                     "  FOREIGN KEY(entry_id) REFERENCES entries(id) ON DELETE CASCADE)");
            db_.exec("INSERT INTO entry_sources_v3 SELECT entry_id, toplevel, source FROM entry_sources");
            db_.exec("DROP TABLE entry_sources");
            db_.exec("ALTER TABLE entry_sources_v3 RENAME TO entry_sources");
            db_.exec("CREATE INDEX IF NOT EXISTS entry_sources_toplevel ON entry_sources(toplevel)");
            tx.commit();
        }
    }

    // entries.root_sha portable-identity index (column defined in kSchema). Kept out
    // of kSchema historically so it ran after a legacy ALTER; now just created here
    // unconditionally (IF NOT EXISTS) — permanent, not a migration.
    db_.exec("CREATE INDEX IF NOT EXISTS entries_root_sha ON entries(root_sha) "
             "WHERE root_sha IS NOT NULL");

    // Note: entries_vec is populated by put() during indexing. For pre-vec0 caches,
    // just delete ~/.gogol/cache.db - it's derived/rebuildable. No migration needed.
    
    // Open the notes.db side (the note content of record) read-write and apply its
    // schema. An explicitly-empty notes_path => cache-only store (note ops no-op) —
    // used by the internal one-time schema migration (ensure_schema_ro), which must
    // NOT create a notes sibling. The single-arg ctor derives a path before delegating
    // here, so tests/one-shot opens still get a working notes side.
    if (!notes_path.empty()) {
        notes_db_ = std::make_unique<Db>(notes_path);
        // Same ordering fix as ensure_notes_schema_ro: fill missing columns on a legacy
        // `notes` table BEFORE exec(kNotesSchema), whose index on notes(supersedes) would
        // otherwise throw `no such column` on a pre-supersedes db.
        {
            bool table_exists = false, has_scope_id = false, has_supersedes = false;
            Db::Stmt ti(*notes_db_, "PRAGMA table_info(notes)");
            while (ti.step()) {
                table_exists = true;
                std::string column = ti.column_text(1);
                if (column == "scope_id") has_scope_id = true;
                if (column == "supersedes") has_supersedes = true;
            }
            if (table_exists) {
                if (!has_scope_id)   notes_db_->exec("ALTER TABLE notes ADD COLUMN scope_id TEXT");
                if (!has_supersedes) notes_db_->exec("ALTER TABLE notes ADD COLUMN supersedes TEXT");
            }
        }
        notes_db_->exec(kNotesSchema);

        // One-time migration: populate notes_vec from notes table for pre-vec0 notes.
        {
            int64_t notes_with_emb = 0, vec_count = 0;
            { Db::Stmt q(*notes_db_, "SELECT COUNT(*) FROM notes WHERE embedding IS NOT NULL AND id NOT IN (SELECT supersedes FROM notes WHERE supersedes IS NOT NULL)");
              if (q.step()) notes_with_emb = q.column_int64(0); }
            { Db::Stmt q(*notes_db_, "SELECT COUNT(*) FROM notes_vec");
              if (q.step()) vec_count = q.column_int64(0); }
            if (notes_with_emb > 0 && vec_count < notes_with_emb) {
                // Notes store embeddings as f16, need to convert to f32 for vec0.
                // Do row-by-row since we need to unpack the f16 blobs.
                Db::Stmt sel(*notes_db_, "SELECT id, embedding FROM notes WHERE embedding IS NOT NULL AND id NOT IN (SELECT supersedes FROM notes WHERE supersedes IS NOT NULL)");
                while (sel.step()) {
                    std::string id = sel.column_text(0);
                    // Check if already in vec0
                    Db::Stmt chk(*notes_db_, "SELECT 1 FROM notes_vec WHERE id=?");
                    chk.bind(1, id);
                    if (chk.step()) continue;  // already migrated
                    
                    const void* blob = sqlite3_column_blob(sel.raw(), 1);
                    int blob_size = sqlite3_column_bytes(sel.raw(), 1);
                    if (!blob || blob_size == 0) continue;
                    
                    // Convert f16 to f32
                    size_t n_floats = blob_size / sizeof(uint16_t);
                    std::vector<float> f32(n_floats);
                    const uint16_t* f16 = static_cast<const uint16_t*>(blob);
                    for (size_t i = 0; i < n_floats; i++) f32[i] = f16_to_float(f16[i]);
                    
                    Db::Stmt ins(*notes_db_,
                        "INSERT OR IGNORE INTO notes_vec(id, embedding) VALUES(?, ?)");
                    ins.bind(1, id);
                    ins.bind(2, f32.data(), int(f32.size() * sizeof(float)));
                    ins.step();
                }
            }
        }
    }
}



void Store::begin() {
    if (!in_tx_) { db_.exec("BEGIN"); in_tx_ = true; }
}

void Store::commit() {
    if (in_tx_) { db_.exec("COMMIT"); in_tx_ = false; }
}

std::string Store::put(const std::string &content, const std::vector<float> &embedding,
                          const std::vector<std::string> &indexes, const std::string &source,
                          const std::string &toplevel) {
    return put(content, embedding, indexes, source, Loc{}, toplevel);
}

std::string Store::put_note(const std::string &content, const std::vector<float> &embedding,
                               const std::string &scope_path,
                               const std::string &supersedes) {
    // Notes live in notes.db (the content of record), NOT cache.db. Co-locate the
    // embedding with the content so a cache rebuild leaves notes fully queryable.
    if (!notes_db_) return ""; // cache-only store: no notes side
    std::string id = content_id(content);
    std::vector<float> v = embedding;
    l2_normalize(v);
    std::vector<uint16_t> half(v.size());
    for (size_t i = 0; i < v.size(); i++) half[i] = float_to_f16(v[i]);
    // Resolve the scope to the note model (ref + relative subdir | absolute | global).
    // scope_id = repo ref; scope_path holds the RELATIVE subpath when a ref resolved
    // (""=whole repo => stored NULL), else the ABSOLUTE path (non-repo/multi-root), else
    // NULL (global). So scope_path's meaning is conditional on scope_id (see schema).
    std::string scope_id, scope_stored;
    if (!scope_path.empty()) {
        auto ns = gogol::resolve_note_scope(scope_path);
        if (!ns.ref.empty()) { scope_id = ns.ref; scope_stored = ns.sub; }  // repo: rel sub
        else scope_stored = ns.abs;                                         // non-repo: abs
    }
    Db::Stmt ins(*notes_db_,
        "INSERT INTO notes(id,content,embedding,scope_path,scope_id,created,supersedes) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  scope_path=COALESCE(excluded.scope_path, notes.scope_path),"
        "  scope_id=COALESCE(excluded.scope_id, notes.scope_id),"
        "  supersedes=COALESCE(excluded.supersedes, notes.supersedes)");
    ins.bind(1, id);
    ins.bind(2, content);
    ins.bind(3, half.data(), int(half.size() * sizeof(uint16_t)));
    if (scope_stored.empty()) ins.bind_null(4); else ins.bind(4, scope_stored);
    if (scope_id.empty()) ins.bind_null(5); else ins.bind(5, scope_id);
    ins.bind(6, int64_t(std::time(nullptr)));
    // A note can't supersede itself (identical-content consolidate yields the same id);
    // binding it would overwrite the real predecessor link via the COALESCE above.
    if (supersedes.empty() || supersedes == id) ins.bind_null(7); else ins.bind(7, supersedes);
    ins.step();
    
    // Insert into notes_vec for ANN search
    {
        Db::Stmt vec_ins(*notes_db_,
            "INSERT OR REPLACE INTO notes_vec(id, embedding) VALUES(?, ?)");
        vec_ins.bind(1, id);
        vec_ins.bind(2, v.data(), int(v.size() * sizeof(float)));
        vec_ins.step();
    }
    return id;
}

std::string Store::put(const std::string &content, const std::vector<float> &embedding,
                          const std::vector<std::string> &indexes, const std::string &source,
                          const Loc &loc, const std::string &toplevel, const std::string &root_sha) {
    // A note (no external source) belongs in notes.db, not cache.db. Route it there
    // so the split holds regardless of which put() overload a caller uses. Index
    // membership is meaningless for a scope-based note, so it's dropped here.
    if (source.empty()) return put_note(content, embedding, /*scope_path=*/"");
    std::string id = content_id(content);
    std::vector<float> v = embedding;
    l2_normalize(v);
    std::vector<uint16_t> half(v.size());
    for (size_t i = 0; i < v.size(); i++) half[i] = float_to_f16(v[i]);

    // idx = the repo/index this entry belongs to (1:1 under the git-path model). If
    // several names are passed (legacy multi-index callers), the FIRST non-empty wins;
    // a later put of the same content can re-home it (COALESCE keeps an existing idx
    // only when the new one is null).
    std::string idx;
    for (const auto &name : indexes) if (!name.empty()) { idx = name; break; }

    Db::Stmt ins(db_,
        "INSERT INTO entries(id,content,embedding,source,idx,root_sha,"
        "line,end_line,symbol,complexity,lines,params,returns,max_depth) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        // INVARIANT §1: re-put must BACKFILL, not no-op. ON CONFLICT DO UPDATE +
        // COALESCE(excluded, old) so a re-sync populates columns added after the row
        // was first written (INSERT OR IGNORE no-op'd on content-id dedup — bug 3564352).
        "ON CONFLICT(id) DO UPDATE SET "
        "  idx=COALESCE(excluded.idx, entries.idx),"
        "  root_sha=COALESCE(excluded.root_sha, entries.root_sha),"
        "  line=COALESCE(excluded.line, entries.line),"
        "  end_line=COALESCE(excluded.end_line, entries.end_line),"
        "  symbol=COALESCE(excluded.symbol, entries.symbol),"
        "  complexity=COALESCE(excluded.complexity, entries.complexity),"
        "  lines=COALESCE(excluded.lines, entries.lines),"
        "  params=COALESCE(excluded.params, entries.params),"
        "  returns=COALESCE(excluded.returns, entries.returns),"
        "  max_depth=COALESCE(excluded.max_depth, entries.max_depth)");
    ins.bind(1, id);
    ins.bind(2, content);
    ins.bind(3, half.data(), int(half.size() * sizeof(uint16_t)));
    if (source.empty()) ins.bind_null(4); else ins.bind(4, source);
    if (idx.empty()) ins.bind_null(5); else ins.bind(5, idx);
    if (root_sha.empty()) ins.bind_null(6); else ins.bind(6, root_sha);
    if (loc.line > 0) ins.bind(7, int64_t(loc.line)); else ins.bind_null(7);
    if (loc.end_line > 0) ins.bind(8, int64_t(loc.end_line)); else ins.bind_null(8);
    if (loc.symbol.empty()) ins.bind_null(9); else ins.bind(9, loc.symbol);
    // Metric columns: bound only when this chunk carries a function's metric; else
    // NULL (a doc/markdown chunk, or a code chunk with no matched function).
    if (loc.has_metric) {
        ins.bind(10, int64_t(loc.complexity));
        ins.bind(11, int64_t(loc.lines));
        ins.bind(12, int64_t(loc.params));
        ins.bind(13, int64_t(loc.returns));
        ins.bind(14, int64_t(loc.max_depth));
    } else {
        ins.bind_null(10); ins.bind_null(11); ins.bind_null(12);
        ins.bind_null(13); ins.bind_null(14);
    }
    ins.step();

    // Insert into vec0 ANN index for fast KNN queries.
    // INVARIANT §1: entries_vec (vec0) has NO ON DELETE CASCADE — every delete path
    // must hand-delete the matching vec row, or a dead 768-dim vector leaks and every
    // KNN scan still touches it (bug 28e1b25). Keep in sync with remove_by_source /
    // remove_stale_chunks.
    {
        Db::Stmt vec_ins(db_,
            "INSERT OR REPLACE INTO entries_vec(id, embedding) VALUES(?, ?)");
        vec_ins.bind(1, id);
        vec_ins.bind(2, v.data(), int(v.size() * sizeof(float)));
        vec_ins.step();
    }

    // Record the toplevel->source mapping for per-checkout scoping. Same entry can
    // exist in multiple toplevels (main + worktree); each gets its own source path.
    if (!toplevel.empty() && !source.empty()) {
        Db::Stmt src_ins(db_,
            "INSERT INTO entry_sources(entry_id, toplevel, source) VALUES(?,?,?) "
            "ON CONFLICT(entry_id, toplevel, source) DO NOTHING");
        src_ins.bind(1, id);
        src_ins.bind(2, toplevel);
        src_ins.bind(3, source);
        src_ins.step();
    }
    return id;
}

void Store::add_membership(const std::string &id, const std::string &index_name) {
    if (index_name.empty()) return;
    // 1:1 membership (git-path model): set the entry's idx column. Overwrites so a
    // touch/re-home updates the entry's owning repo. No junction table.
    Db::Stmt s(db_, "UPDATE entries SET idx=? WHERE id=?");
    s.bind(1, index_name);
    s.bind(2, id);
    s.step();
}

bool Store::has_entry(const std::string &id) const {
    auto& s = stmt_has_entry();
    s.reset();
    s.bind(1, id);
    return s.step();
}

bool Store::writer_busy() {
    // Probe for a concurrent writer without blocking. The calling store may be opened
    // READ-ONLY (query path), which cannot BEGIN IMMEDIATE, so we open our own
    // short-lived writable connection to the same file purely to test the lock.
    // busy_timeout=0 makes BEGIN IMMEDIATE fail fast with SQLITE_BUSY when another
    // writer holds the lock, instead of waiting out the normal 5s. On success we got
    // the lock ourselves (no other writer) — roll back at once so we never write.
    sqlite3 *h = nullptr;
    if (sqlite3_open(db_path_.c_str(), &h) != SQLITE_OK || !h) {
        if (h) sqlite3_close(h);
        return false;  // can't probe — assume free, let the normal path handle it
    }
    sqlite3_busy_timeout(h, 0);
    int rc = sqlite3_exec(h, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    bool busy = (rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    if (rc == SQLITE_OK) {
        sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    sqlite3_close(h);
    return busy;
}

// Cached statement accessors - lazy initialization
Db::Stmt& Store::stmt_has_entry() const {
    if (!stmt_has_entry_) {
        stmt_has_entry_ = std::make_unique<Db::Stmt>(db_, "SELECT 1 FROM entries WHERE id=?");
    }
    return *stmt_has_entry_;
}

Db::Stmt& Store::stmt_add_edge() const {
    if (!stmt_add_edge_) {
        stmt_add_edge_ = std::make_unique<Db::Stmt>(db_, 
            "INSERT OR IGNORE INTO edges(kind,src_sym,dst_sym,idx,file) VALUES(?,?,?,?,?)");
    }
    return *stmt_add_edge_;
}

Db::Stmt& Store::stmt_set_file_hash() const {
    if (!stmt_set_file_hash_) {
        stmt_set_file_hash_ = std::make_unique<Db::Stmt>(db_,
            "INSERT INTO file_hashes(idx,source,hash) VALUES(?,?,?) "
            "ON CONFLICT(idx,source) DO UPDATE SET hash=excluded.hash");
    }
    return *stmt_set_file_hash_;
}

void Store::update_entry_loc(const std::string &id, const std::string &index_name, const Loc &loc,
                        const std::string &toplevel) {
    // Refresh location (COALESCE keeps existing where the new is null) + membership,
    // no embedding. Mirrors put()'s non-embed side-effects for an unchanged chunk.
    Db::Stmt u(db_,
        "UPDATE entries SET "
        "  line=COALESCE(?, line), end_line=COALESCE(?, end_line), "
        "  symbol=COALESCE(?, symbol), "
        "  complexity=COALESCE(?, complexity), lines=COALESCE(?, lines), "
        "  params=COALESCE(?, params), returns=COALESCE(?, returns), "
        "  max_depth=COALESCE(?, max_depth) "
        "WHERE id=?");
    if (loc.line > 0) u.bind(1, int64_t(loc.line)); else u.bind_null(1);
    if (loc.end_line > 0) u.bind(2, int64_t(loc.end_line)); else u.bind_null(2);
    if (loc.symbol.empty()) u.bind_null(3); else u.bind(3, loc.symbol);
    if (loc.has_metric) {
        u.bind(4, int64_t(loc.complexity)); u.bind(5, int64_t(loc.lines));
        u.bind(6, int64_t(loc.params)); u.bind(7, int64_t(loc.returns));
        u.bind(8, int64_t(loc.max_depth));
    } else {
        u.bind_null(4); u.bind_null(5); u.bind_null(6); u.bind_null(7); u.bind_null(8);
    }
    u.bind(9, id);
    u.step();
    add_membership(id, index_name);

    // vec0 migration is handled by bulk INSERT OR IGNORE on store open (Store ctor),
    // not per-entry here - that was too slow (O(n) SELECTs).

    // Record toplevel->source mapping (worktree support). Need the source from entries
    // to complete the entry_sources row. If toplevel is set, upsert the mapping.
    if (!toplevel.empty()) {
        Db::Stmt src(db_, "SELECT source FROM entries WHERE id=?");
        src.bind(1, id);
        if (src.step()) {
            std::string source = src.column_text(0);
            if (!source.empty()) {
                Db::Stmt src_ins(db_,
                    "INSERT INTO entry_sources(entry_id, toplevel, source) VALUES(?,?,?) "
                    "ON CONFLICT(entry_id, toplevel, source) DO NOTHING");
                src_ins.bind(1, id);
                src_ins.bind(2, toplevel);
                src_ins.bind(3, source);
                src_ins.step();
            }
        }
    }
}

std::map<std::string, uint64_t> Store::file_hashes(const std::string &idx) const {
    std::map<std::string, uint64_t> out;
    Db::Stmt s(db_, "SELECT source, hash FROM file_hashes WHERE idx=?");
    s.bind(1, idx);
    while (s.step()) out[s.column_text(0)] = (uint64_t)s.column_int64(1);
    return out;
}

void Store::set_file_hash(const std::string &idx, const std::string &source, uint64_t hash) {
    auto& s = stmt_set_file_hash();
    s.reset();
    s.bind(1, idx);
    s.bind(2, source);
    s.bind(3, (int64_t)hash);
    s.step();
}

void Store::set_file_hashes_batch(const std::string &idx,
                                  const std::map<std::string, uint64_t> &hashes) {
    // Batch insert/update file hashes using the cached prepared statement
    auto& s = stmt_set_file_hash();
    for (const auto &[source, hash] : hashes) {
        s.reset();
        s.bind(1, idx);
        s.bind(2, source);
        s.bind(3, (int64_t)hash);
        s.step();
    }
}

std::vector<std::string> Store::indexes_for(const std::string &id) const {
    // 1:1 membership: the entry's idx column (0 or 1 names). Returned as a vector to
    // keep the Entry.indexes shape stable for callers.
    std::vector<std::string> out;
    Db::Stmt s(db_, "SELECT idx FROM entries WHERE id=? AND idx IS NOT NULL AND idx<>''");
    s.bind(1, id);
    while (s.step()) out.push_back(s.column_text(0));
    return out;
}

std::optional<Entry> Store::get(const std::string &id) const {
    Db::Stmt s(db_, "SELECT content, source, line, end_line, symbol FROM entries WHERE id=?");
    s.bind(1, id);
    if (!s.step()) {
        // Not a cache.db entry — try notes.db (a note's content of record).
        if (notes_db_) {
            Db::Stmt ns(*notes_db_, "SELECT content, scope_path, supersedes FROM notes WHERE id=?");
            ns.bind(1, id);
            if (ns.step()) {
                Entry e;
                e.id = id;
                e.content = ns.column_text(0);
                e.source = "";           // a note has no external source
                e.type = "note";
                std::string supersedes = ns.column_text(2);
                if (!supersedes.empty()) e.supersedes = supersedes;
                Db::Stmt superseded_by_q(*notes_db_, "SELECT id FROM notes WHERE supersedes=? LIMIT 1");
                superseded_by_q.bind(1, id);
                if (superseded_by_q.step()) e.superseded_by = superseded_by_q.column_text(0);
                return e;
            }
        }
        return std::nullopt;
    }
    Entry e;
    e.id = id;
    e.content = s.column_text(0);
    e.source = s.column_text(1);
    e.line = (int)s.column_int64(2);
    e.end_line = (int)s.column_int64(3);
    e.symbol = s.column_text(4);
    for (const auto &name : indexes_for(id)) e.indexes.push_back(name);
    e.type = type_from_source(e.source);
    return e;
}

std::optional<Entry> Store::get_latest(const std::string &id) const {
    auto e = get(id);
    std::set<std::string> seen{id};  // guard: identical-content consolidate can self-link
    while (e && !e->superseded_by.empty() && seen.insert(e->superseded_by).second) {
        auto next = get(e->superseded_by);
        if (!next) break;
        e = std::move(next);
    }
    if (e && e->id != id) e->resolved_from = id;
    return e;
}

std::string type_from_source(const std::string &source) {
    if (source.empty()) return "note";                    // LLM-authored: no external source
    // Otherwise source is an absolute filesystem path (docs/code). Type by ext.
    if (source.size() > 3 && source.compare(source.size() - 3, 3, ".md") == 0) return "doc";
    return "code";
}

int64_t Store::count_by_type(const std::string &type) const {
    // Notes live in notes.db now; code/doc live in cache.db (no notes there anymore).
    if (type == "note") {
        if (!notes_db_) return 0;
        Db::Stmt q(*notes_db_, "SELECT COUNT(*) FROM notes");
        return q.step() ? q.column_int64(0) : 0;
    }
    int64_t n = 0;
    Db::Stmt s(db_, "SELECT source FROM entries WHERE source IS NOT NULL");
    while (s.step()) {
        if (type_from_source(s.column_text(0)) == type) n++;
    }
    return n;
}

bool Store::note_scope_matches(const std::string &note_scope_id,
                                  const std::string &note_scope_sub,
                                  const std::string &query_ref,
                                  const std::string &query_sub,
                                  const std::string &query_abs) {
    // Prefix helper: is `child` at or under `parent` on a path boundary? (""=parent
    // means the whole repo / no narrowing -> always under.) "/a/b" is under "/a" and
    // under "/a/b", but "/a/bc" is NOT under "/a/b".
    auto under = [](const std::string &parent, const std::string &child) {
        if (parent.empty()) return true;
        std::string p = parent;
        if (!p.empty() && p.back() == '/') p.pop_back();
        if (child == p) return true;
        return child.size() > p.size() &&
               child.compare(0, p.size(), p) == 0 && child[p.size()] == '/';
    };
    // GLOBAL note (no id, no sub) -> always surfaces.
    if (note_scope_id.empty() && note_scope_sub.empty()) return true;
    // REPO-scoped note (has a ref): same repo AND query subpath under the note's subdir.
    if (!note_scope_id.empty())
        return note_scope_id == query_ref && under(note_scope_sub, query_sub);
    // NON-REPO note (no ref, absolute path in scope_sub): query abs path under it.
    return under(note_scope_sub, query_abs);
}

std::vector<std::string> Store::ids_of_type(const std::string &type) const {
    // INVARIANT §3 (docs/INVARIANTS.md): --type is a PRE-filter on the candidate set,
    // not a post-slice of the top-k. Cosine is scored outside SQL, so a filter that
    // must shrink the search space runs here, before KNN scoring — as a post-filter,
    // ~300 notes never beat ~68k code/doc into the global top-k (bug 32a35e2).
    // Type is derived from source: note = NULL, doc = *.md, code = other non-null.
    std::string where =
        type == "note" ? "source IS NULL" :
        type == "doc"  ? "source LIKE '%.md'" :
        type == "code" ? "source IS NOT NULL AND source NOT LIKE '%.md'" : "";
    std::vector<std::string> out;
    if (where.empty()) return out;
    Db::Stmt s(db_, "SELECT id FROM entries WHERE " + where);
    while (s.step()) out.push_back(s.column_text(0));
    return out;
}

std::vector<std::string> Store::ids_in_indexes(const std::vector<std::string> &names) const {
    // Membership filter: entries belonging to ANY of `names` (bare index name; an
    // "index:" prefix is tolerated and stripped). Empty => all entries.
    std::vector<std::string> out;
    std::vector<std::string> bare;
    bare.reserve(names.size());
    for (auto n : names) {
        if (n.rfind("index:", 0) == 0) n = n.substr(6);
        if (!n.empty()) bare.push_back(std::move(n));
    }
    if (bare.empty()) {
        Db::Stmt s(db_, "SELECT id FROM entries");
        while (s.step()) out.push_back(s.column_text(0));
        return out;
    }
    std::string ph;
    for (size_t i = 0; i < bare.size(); i++) ph += (i ? ",?" : "?");
    // A name is either a path (idx) or a portable root-SHA token (root_sha). Match
    // EITHER column so a container's SHA scope resolves to that repo's entries just as
    // a host's path scope does. The two placeholder sets are bound to the same values.
    Db::Stmt s(db_, "SELECT id FROM entries WHERE idx IN (" + ph + ") "
                    "OR root_sha IN (" + ph + ")");
    int i = 1;
    for (const auto &n : bare) s.bind(i++, n);
    for (const auto &n : bare) s.bind(i++, n);
    while (s.step()) out.push_back(s.column_text(0));
    return out;
}

std::vector<gogol::IndexLoc> Store::index_locations() const {
    // Each distinct idx (SHA) left-joined to its most-recent sighting path. A repo with
    // no sighting (legacy path-keyed idx, or a ref never recorded) falls back to the idx
    // string as its own path — preserving the old "match name/suffix against idx" path.
    // MAX(last_seen) picks the freshest checkout when a ref has been seen at several.
    std::vector<gogol::IndexLoc> out;
    Db::Stmt s(db_,
        "SELECT e.idx, COALESCE(rs.path, e.idx) "
        "FROM (SELECT DISTINCT idx FROM entries WHERE idx IS NOT NULL AND idx <> '') e "
        "LEFT JOIN ref_sightings rs ON rs.root_sha = e.idx "
        "  AND rs.last_seen = (SELECT MAX(last_seen) FROM ref_sightings WHERE root_sha = e.idx)");
    while (s.step()) out.push_back({s.column_text(0), s.column_text(1)});
    return out;
}

std::string Store::index_display_path(const std::string &idx) const {
    if (idx.empty()) return idx;
    // Prefer the location layer (ref_sightings): a live path sighted for this ref is
    // the best display path (T4/R6). Fall back to the entry_sources toplevel scan, then
    // the idx itself. "Live" = still exists on disk (fs::exists, microseconds).
    {
        Db::Stmt s(db_,
            "SELECT path FROM ref_sightings WHERE root_sha=? ORDER BY last_seen DESC");
        s.bind(1, idx);
        while (s.step()) {
            std::string p = s.column_text(0);
            std::error_code ec;
            if (std::filesystem::exists(p, ec) && !ec) return p;  // live sighting wins
        }
        // No live sighting: fall through to the entry_sources scan (a stale sighting
        // must not beat a live checkout recorded there).
    }
    // Fallback: a representative checkout path from entry_sources (pre-sightings data,
    // or a no-SHA fallback idx that is already a path).
    Db::Stmt s(db_,
        "SELECT DISTINCT es.toplevel FROM entry_sources es "
        "JOIN entries e ON e.id = es.entry_id WHERE e.idx=?");
    s.bind(1, idx);
    std::string first;
    while (s.step()) {
        std::string top = s.column_text(0);
        if (first.empty()) first = top;
        std::error_code ec;
        if (std::filesystem::exists(top, ec) && !ec) return top;  // prefer a live checkout
    }
    return first.empty() ? idx : first;
}

std::string Store::display_source(const std::string &idx, const std::string &rel) const {
    if (rel.empty()) return rel;                 // note / no source
    if (!rel.empty() && rel[0] == '/') return rel; // already absolute (legacy/unmigrated)
    if (idx.empty()) return rel;
    std::string disp = index_display_path(idx);
    if (disp.empty() || disp == idx) return rel; // couldn't resolve a checkout path
    return disp + "/" + rel;
}

void Store::record_sighting(const std::string &root_sha, const std::string &path) {
    if (root_sha.empty() || path.empty()) return;
    // Best-effort (R2): a read command must never fail/block on learning a location.
    // A read-only store throws on write; swallow it. Preserve an existing overlay flag.
    try {
        Db::Stmt s(db_,
            "INSERT INTO ref_sightings(root_sha, path, last_seen, overlay) VALUES(?,?,?,0) "
            "ON CONFLICT(root_sha, path) DO UPDATE SET last_seen=excluded.last_seen");
        s.bind(1, root_sha);
        s.bind(2, path);
        s.bind(3, int64_t(std::time(nullptr)));
        s.step();
    } catch (...) {
        // read-only store, or write-lock contention: skip silently (R2/SC5).
    }
}

std::vector<Store::Sighting> Store::sightings_for(const std::string &root_sha) const {
    std::vector<Sighting> out;
    if (root_sha.empty()) return out;
    Db::Stmt s(db_,
        "SELECT root_sha, path, last_seen, overlay FROM ref_sightings "
        "WHERE root_sha=? ORDER BY last_seen DESC");
    s.bind(1, root_sha);
    while (s.step())
        out.push_back({s.column_text(0), s.column_text(1), s.column_int64(2),
                       s.column_int64(3) != 0});
    return out;
}

std::vector<Store::Sighting> Store::sightings_under(const std::string &dir) const {
    // Sightings whose path is under `dir` on a path boundary (so "/a/b" matches
    // "/a/b/c" but not "/a/bc"), returned with the path so the caller stat-checks what
    // it already fetched (no per-ref re-query). Uses the ref_sightings_path index via a
    // range scan on the prefix; the exact boundary is re-checked in C++ to drop
    // siblings like "/a/bc" that fall in the range.
    std::vector<Sighting> out;
    if (dir.empty()) return out;
    std::string pfx = dir;
    if (!pfx.empty() && pfx.back() == '/') pfx.pop_back();
    // Two index-usable ranges: the exact dir (pfx itself) and its children (paths
    // starting pfx + '/'). The children range is [pfx+"/", pfx+"0") — '0' is '/'+1, the
    // smallest char after the separator — so it captures exactly pfx/<anything> and
    // excludes siblings like pfx+"c". No C++ boundary re-check needed.
    std::string lo = pfx + "/";
    std::string hi = pfx + "0";  // '0' == '/' + 1
    Db::Stmt s(db_,
        "SELECT root_sha, path, last_seen, overlay FROM ref_sightings "
        "WHERE path = ? OR (path >= ? AND path < ?)");
    s.bind(1, pfx);
    s.bind(2, lo);
    s.bind(3, hi);
    while (s.step())
        out.push_back({s.column_text(0), s.column_text(1), s.column_int64(2),
                       s.column_int64(3) != 0});
    return out;
}

bool Store::set_overlay(const std::string &root_sha, const std::string &path, bool on) {
    if (root_sha.empty() || path.empty()) return false;
    Db::Stmt s(db_,
        "INSERT INTO ref_sightings(root_sha, path, last_seen, overlay) VALUES(?,?,?,?) "
        "ON CONFLICT(root_sha, path) DO UPDATE SET overlay=excluded.overlay, "
        "last_seen=excluded.last_seen");
    s.bind(1, root_sha);
    s.bind(2, path);
    s.bind(3, int64_t(std::time(nullptr)));
    s.bind(4, int64_t(on ? 1 : 0));
    s.step();
    return true;
}

std::vector<Store::Sighting> Store::active_overlays() const {
    std::vector<Sighting> out;
    Db::Stmt s(db_,
        "SELECT root_sha, path, last_seen, overlay FROM ref_sightings WHERE overlay=1");
    while (s.step())
        out.push_back({s.column_text(0), s.column_text(1), s.column_int64(2), true});
    return out;
}

std::vector<std::string> Store::recall_restrict_refs(const std::string &dir,
                                                     bool base_scoped) const {
    std::vector<std::string> refs;
    std::set<std::string> seen;
    auto add_live = [&](const std::string &ref, const std::string &path) {
        if (ref.empty() || seen.count(ref)) return;
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) { refs.push_back(ref); seen.insert(ref); }
        // Not marked seen when dead: a later live sighting of the same ref (e.g. its
        // overlay path) can still add it.
    };
    // Parent-dir search: refs sighted under `dir`, stat-checked via the path we already
    // fetched (no per-ref re-query).
    if (!dir.empty())
        for (const auto &s : sightings_under(dir)) add_live(s.root_sha, s.path);
    // Overlay union only when already a restriction (scoped repo OR parent-dir refs):
    // never widen a global search into an overlay-only one.
    if (base_scoped || !refs.empty())
        for (const auto &ov : active_overlays()) add_live(ov.root_sha, ov.path);
    return refs;
}

std::vector<Hit> Store::query(const std::vector<float> &qvec, int k,
                                 const std::vector<std::string> &restrict_all,
                                 const std::vector<std::string> &restrict_any,
                                 const std::string &type,
                                 const std::string &scope_path,
                                 const std::vector<std::string> *precomputed_ids) const {
    // vec0 KNN search (entries_vec + notes_vec), then merge/filter the results.
    std::vector<float> q = qvec;
    l2_normalize(q);
    
    bool have_restrict = !restrict_all.empty() || !restrict_any.empty();
    bool want_notes = type.empty() || type == "note";
    bool want_cache = type.empty() || type == "code" || type == "doc";
    
    // Build candidate id sets for filtering (if needed)
    std::unordered_set<std::string> allowed_ids;
    bool has_filter = false;
    if (want_cache && (have_restrict || type == "code" || type == "doc")) {
        has_filter = true;
        std::vector<std::string> ids_filter;
        if (have_restrict) {
            if (precomputed_ids && !precomputed_ids->empty()) {
                ids_filter = *precomputed_ids;  // reuse caller's ids_in_indexes result
            } else {
                std::vector<std::string> names;
                names.insert(names.end(), restrict_all.begin(), restrict_all.end());
                names.insert(names.end(), restrict_any.begin(), restrict_any.end());
                ids_filter = ids_in_indexes(names);
            }
        }
        if (type == "code" || type == "doc") {
            auto tids = ids_of_type(type);
            if (have_restrict) {
                std::set<std::string> ts(tids.begin(), tids.end());
                for (auto &id : ids_filter) 
                    if (ts.count(id)) allowed_ids.insert(id);
            } else {
                allowed_ids.insert(tids.begin(), tids.end());
            }
        } else {
            allowed_ids.insert(ids_filter.begin(), ids_filter.end());
        }
    }
    
    std::vector<Hit> hits;
    
    // Query cache entries via vec0 KNN
    if (want_cache) {
        // Over-fetch to account for filtering
        int fetch_k = has_filter ? k * 4 : k;
        Db::Stmt s(db_,
            "SELECT id, distance FROM entries_vec "
            "WHERE embedding MATCH ? ORDER BY distance LIMIT ?");
        s.bind(1, q.data(), int(q.size() * sizeof(float)));
        s.bind(2, int64_t(fetch_k));
        while (s.step()) {
            std::string id = s.column_text(0);
            if (has_filter && !allowed_ids.count(id)) continue;
            float dist = static_cast<float>(s.column_double(1));
            // Convert distance to similarity score (cosine distance -> similarity)
            // vec0 cosine distance is 1 - cosine_similarity, so similarity = 1 - distance
            float score = 1.0f - dist;
            hits.push_back({id, score});
            if (!has_filter && (int)hits.size() >= k) break;
        }
    }
    
    // Query notes via vec0 KNN (scope-filtered)
    // Notes scope by the three-shape model (global | repo-ref+subdir | non-repo abs).
    // Resolve the query's `scope_path` ONCE to (ref, sub, abs): if it is a bare root
    // SHA (from --ref), it's (ref, "", "") — repo match, no subdir narrowing, no abs;
    // otherwise resolve the path to its repo (ref + relative subdir) or absolute.
    if (want_notes && notes_db_) {
        std::string query_ref, query_sub, query_abs;
        if (gogol::looks_like_root_sha(scope_path)) {
            query_ref = scope_path;                 // --ref: repo id, no subpath/abs
        } else if (!scope_path.empty()) {
            auto ns = gogol::resolve_note_scope(scope_path);
            query_ref = ns.ref; query_sub = ns.sub;
            query_abs = ns.abs.empty() ? scope_path : ns.abs;  // fall back to raw path
        }
        Db::Stmt s(*notes_db_,
            "SELECT id, distance FROM notes_vec "
            "WHERE embedding MATCH ? ORDER BY distance LIMIT ?");
        s.bind(1, q.data(), int(q.size() * sizeof(float)));
        s.bind(2, int64_t(k));
        while (s.step()) {
            std::string id = s.column_text(0);
            // Archived notes must stay excluded from live recall even if a stale row
            // survives in notes_vec (for example from an old rebuild or migration).
            Db::Stmt archived_q(*notes_db_, "SELECT 1 FROM notes WHERE supersedes=? LIMIT 1");
            archived_q.bind(1, id);
            if (archived_q.step()) continue;
            Db::Stmt scope_q(*notes_db_, "SELECT scope_path, scope_id FROM notes WHERE id=?");
            scope_q.bind(1, id);
            if (scope_q.step()) {
                std::string note_sub = scope_q.column_text(0);
                std::string note_scope_id = scope_q.column_text(1);
                if (!note_scope_matches(note_scope_id, note_sub, query_ref, query_sub, query_abs))
                    continue;
            }
            float dist = static_cast<float>(s.column_double(1));
            float score = 1.0f - dist;
            hits.push_back({id, score});
        }
    }
    
    // Sort combined results by score descending and take top-k
    int kk = std::min<int>(k, int(hits.size()));
    std::partial_sort(hits.begin(), hits.begin() + kk, hits.end(),
                      [](const Hit &a, const Hit &b) { return a.score > b.score; });
    hits.resize(kk);
    return hits;
}

std::vector<float> Store::get_embedding(const std::string &id) const {
    // Retrieve from vec0 index (stored as f32).
    Db::Stmt s(db_, "SELECT embedding FROM entries_vec WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return {};
    const void* blob = s.column_blob(0);
    int bytes = s.column_bytes(0);
    if (!blob || bytes == 0) return {};
    int n = bytes / sizeof(float);
    std::vector<float> v(n);
    std::memcpy(v.data(), blob, bytes);
    return v;
}

std::vector<Hit> Store::related(const std::string &id, int k,
                                const std::string &type) const {
    // 1. Get the entry's embedding
    std::vector<float> emb = get_embedding(id);
    if (emb.empty()) return {};
    
    // 2. Run cosine KNN with type filter
    // Empty type = code + doc (no notes). Explicit "note" = notes only.
    // Explicit "code" or "doc" = that type only.
    std::string filter_type = type;
    if (filter_type.empty()) {
        // Default: code + doc, no notes
        std::vector<Hit> code_hits = query(emb, k, {}, {}, "code", "");
        std::vector<Hit> doc_hits = query(emb, k, {}, {}, "doc", "");
        
        // Merge and sort
        std::vector<Hit> all;
        all.reserve(code_hits.size() + doc_hits.size());
        for (auto& h : code_hits) if (h.id != id) all.push_back(std::move(h));
        for (auto& h : doc_hits) if (h.id != id) all.push_back(std::move(h));
        
        std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) {
            return a.score > b.score;
        });
        if ((int)all.size() > k) all.resize(k);
        return all;
    }
    
    // Single type filter (including "note")
    std::vector<Hit> hits = query(emb, k + 1, {}, {}, filter_type, "");
    // Remove self from results
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&id](const Hit& h) { return h.id == id; }),
               hits.end());
    if ((int)hits.size() > k) hits.resize(k);
    return hits;
}

int Store::forget_note(const std::string &id) {
    // Notes live in notes.db now. This is the ONLY delete surface on notes.db, and
    // it is narrow BY CONSTRUCTION: single id, notes table only. It cannot touch a
    // code/doc entry (those are in cache.db, a different file) — so "refuses non-note"
    // is structural, not just a guard. Returns: 1 removed, 0 not-found, -1 refused.
    if (!notes_db_) return 0; // cache-only store: nothing to forget
    // A cache.db (code/doc) id passed here is not ours to forget — report refused so
    // the caller's message ("delete the source file") still fires.
    {
        Db::Stmt q(db_, "SELECT 1 FROM entries WHERE id=?");
        q.bind(1, id);
        if (q.step()) return -1;
    }
    bool found = false;
    { Db::Stmt q(*notes_db_, "SELECT 1 FROM notes WHERE id=?"); q.bind(1, id);
      if (q.step()) found = true; }
    if (!found) return 0;
    { Db::Stmt d(*notes_db_, "DELETE FROM notes WHERE id=?"); d.bind(1, id); d.step(); }
    { Db::Stmt d(*notes_db_, "DELETE FROM notes_vec WHERE id=?"); d.bind(1, id); d.step(); }
    return 1;
}

Store::ConsolidateResult Store::consolidate_note(const std::string &old_id,
                                                   const std::string &content,
                                                   const std::vector<float> &embedding,
                                                   const std::string &scope_path) {
    // Architecture: archive the old note instead of deleting it. The `notes` row stays
    // to preserve history, but the `notes_vec` row is removed so recall stops surfacing it.
    ConsolidateResult out;
    if (!notes_db_) return out;  // cache-only store: nothing to consolidate
    Db::Tx tx(*notes_db_);
    if (!old_id.empty()) {
        Db::Stmt q(db_, "SELECT 1 FROM entries WHERE id=?");
        q.bind(1, old_id);
        if (q.step()) {
            out.refused = true;
        } else {
            Db::Stmt note_q(*notes_db_, "SELECT 1 FROM notes WHERE id=?");
            note_q.bind(1, old_id);
            if (note_q.step()) {
                { Db::Stmt d(*notes_db_, "DELETE FROM notes_vec WHERE id=?"); d.bind(1, old_id); d.step(); }
                out.archived = true;
                out.forgotten = true;
            }
        }
    }
    out.id = put_note(content, embedding, scope_path, old_id);
    tx.commit();
    return out;
}

int64_t Store::remove_by_source(const std::string &source) {
    // INVARIANT §1 (docs/INVARIANTS.md): DELETED-file surface. Must stay symmetric with
    // remove_stale_chunks (changed-file surface). Drop entries + entries_vec (no
    // cascade) + edges (file-keyed) + file_hashes (source-keyed). Add any new
    // id/file/source-keyed satellite table to BOTH surfaces.
    // collect ids first, then delete the entry rows (idx + symbol + metrics are columns
    // on entries now, so they go with the row — no side tables to clean up)
    std::vector<std::string> ids;
    { Db::Stmt q(db_, "SELECT id FROM entries WHERE source=?"); q.bind(1, source);
      while (q.step()) ids.push_back(q.column_text(0)); }
    Db::Tx tx(db_);
    for (const auto &id : ids) {
        { Db::Stmt d(db_, "DELETE FROM entries WHERE id=?"); d.bind(1, id); d.step(); }
        { Db::Stmt d(db_, "DELETE FROM entries_vec WHERE id=?"); d.bind(1, id); d.step(); }
    }
    // Also drop the file's edges — they're file-keyed, so a deleted file's
    // call/import/docref edges would otherwise dangle (point into gone content),
    // poisoning links counts. Same class as orphaned symbol-map rows.
    { Db::Stmt d(db_, "DELETE FROM edges WHERE file=?"); d.bind(1, source); d.step(); }
    { Db::Stmt d(db_, "DELETE FROM file_hashes WHERE source=?"); d.bind(1, source); d.step(); }
    tx.commit();
    return int64_t(ids.size());
}


Store::Stats Store::stats() const {
    Stats s{0, 0, 0, 0};
    { Db::Stmt q(db_, "SELECT COUNT(*) FROM entries"); if (q.step()) s.entries = q.column_int64(0); }
    { Db::Stmt q(db_, "SELECT COUNT(DISTINCT idx) FROM entries WHERE idx IS NOT NULL AND idx<>''"); if (q.step()) s.indexes = q.column_int64(0); }
    { Db::Stmt q(db_, "SELECT COUNT(*) FROM edges"); if (q.step()) s.edges = q.column_int64(0); }
    { Db::Stmt q(db_, "SELECT COUNT(*) FROM entries_vec"); if (q.step()) s.vec_entries = q.column_int64(0); }
    return s;
}

std::vector<Store::IndexInfo> Store::index_breakdown() const {
    // One row per index: entry count, the portable ref, the on-disk checkout dir(s),
    // and whether any checkout is an active overlay.
    //
    // root_sha: report it ONLY when the idx IS a root SHA (idx == root_sha) — i.e. a
    // genuinely SHA-keyed index. A path-keyed / multi-root idx has no single ref, so it
    // reports "". This also avoids a stray value: content dedup can leave a FEW rows of
    // a path-keyed index stamped with another repo's SHA (shared identical content), and
    // a bare MIN(root_sha) would surface that collision as the whole index's ref.
    // paths + overlay: from ref_sightings (the location layer), keyed by the ref.
    std::vector<IndexInfo> out;
    Db::Stmt q(db_,
        "SELECT idx, MAX(CASE WHEN root_sha = idx THEN root_sha END), COUNT(*) "
        "FROM entries WHERE idx IS NOT NULL AND idx<>'' GROUP BY idx ORDER BY COUNT(*) DESC");
    while (q.step()) {
        IndexInfo info;
        info.idx = q.column_text(0);
        info.root_sha = q.column_text(1);  // "" unless the idx is itself a root SHA
        info.entries = q.column_int64(2);
        // Checkout dir(s) + overlay from the location layer. Sightings key on the ref;
        // for a SHA-keyed idx that's info.idx, for a path-keyed idx it's the path idx
        // itself (record_sighting stores whatever identity refresh_repo had).
        const std::string &ref = !info.root_sha.empty() ? info.root_sha : info.idx;
        for (const auto &s : sightings_for(ref))
            info.dirs.push_back({s.path, s.overlay});
        // Fallback: a path-keyed idx (multi-root/legacy) gets no ref_sighting (those are
        // SHA-gated), but the idx IS its on-disk directory — surface it (never an overlay)
        // so `dirs` isn't empty for a perfectly locatable repo.
        if (info.dirs.empty() && !info.idx.empty() && info.idx[0] == '/')
            info.dirs.push_back({info.idx, false});
        out.push_back(std::move(info));
    }
    return out;
}

// leading identifier of a chunk = its local symbol name (matches v1 edge symbols'
// tail). e.g. "handler(event...)" -> "handler"; "APQDynamoCache.get(...)" -> "get".
std::string symbol_tail(const std::string &sym) {
    // strip the last "." OR "::" separator -> final segment.
    auto colons = sym.rfind("::");
    auto dot = sym.rfind('.');
    size_t cut = std::string::npos;
    if (colons != std::string::npos) cut = colons + 2;         // after "::"
    if (dot != std::string::npos && (cut == std::string::npos || dot + 1 > cut))
        cut = dot + 1;                                          // after "." if later
    return cut == std::string::npos ? sym : sym.substr(cut);
}

std::string extract_leading_symbol(const std::string &content) {
    size_t i = 0;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t' || content[i] == '\n')) i++;
    size_t start = i;
    while (i < content.size() &&
           (std::isalnum((unsigned char)content[i]) || content[i] == '_' || content[i] == '.' || content[i] == ':'))
        i++;
    std::string sym = content.substr(start, i - start);
    return symbol_tail(sym);
}

std::string symbol_from_heading(const std::string &heading) {
    // A tree-sitter chunk heading is the REAL symbol name, optionally decorated as
    // "Parent.name(params): ret" (parent qualifier + signature + return type). The
    // bare symbol is everything before the first '(' (drops the signature), tail-cut
    // to strip a "Parent."/"Parent::" qualifier so it matches the call-graph edge
    // keys (which are symbol_tail(find_name(...))). Leading/trailing space trimmed.
    // Unlike extract_leading_symbol(enriched_text), this never sees directory-context
    // tokens, so it yields the function name — not "storage"/"core".
    size_t paren = heading.find('(');
    std::string name = paren == std::string::npos ? heading : heading.substr(0, paren);
    // trim surrounding whitespace
    size_t b = name.find_first_not_of(" \t\n");
    size_t e = name.find_last_not_of(" \t\n");
    if (b == std::string::npos) return "";
    name = name.substr(b, e - b + 1);
    return symbol_tail(name);
}
static std::string leading_symbol(const std::string &content) { return extract_leading_symbol(content); }

void Store::add_edge(const std::string &kind, const std::string &src_sym,
                        const std::string &dst_sym, const std::string &idx,
                        const std::string &file) {
    auto& s = stmt_add_edge();
    s.reset();
    s.bind(1, kind); s.bind(2, src_sym); s.bind(3, dst_sym); s.bind(4, idx);
    if (file.empty()) s.bind_null(5); else s.bind(5, file);
    s.step();
}

int64_t Store::remove_edges_of_file(const std::string &file) {
    int64_t n = 0;
    { Db::Stmt q(db_, "SELECT COUNT(*) FROM edges WHERE file=?"); q.bind(1, file); if (q.step()) n = q.column_int64(0); }
    Db::Stmt d(db_, "DELETE FROM edges WHERE file=?"); d.bind(1, file); d.step();
    return n;
}

int64_t Store::remove_stale_chunks(const std::string &idx, const std::string &source,
                                      const std::vector<std::string> &keep_ids) {
    // INVARIANT §1: CHANGED-file surface. Must stay symmetric with remove_by_source.
    // Drops the file's entries NOT in the current chunk-id set. SCOPED TO idx: a
    // relative source is not unique across repos, so a bare `WHERE source=?` would
    // delete a sibling repo's chunks with the same relative path.
    std::set<std::string> keep(keep_ids.begin(), keep_ids.end());
    std::vector<std::string> stale;
    { Db::Stmt q(db_, "SELECT id FROM entries WHERE idx=? AND source=?");
      q.bind(1, idx); q.bind(2, source);
      while (q.step()) { std::string id = q.column_text(0); if (!keep.count(id)) stale.push_back(id); } }
    if (!stale.empty()) {
        Db::Stmt d(db_, "DELETE FROM entries WHERE id=?");
        Db::Stmt v(db_, "DELETE FROM entries_vec WHERE id=?");
        for (const auto &id : stale) {
            d.reset();
            d.bind(1, id);
            d.step();
            v.reset();
            v.bind(1, id);
            v.step();
        }
    }
    return (int64_t)stale.size();
}

int64_t Store::purge_missing_sources(const std::string &idx, const std::string &root) {
    // INVARIANT §1: deleted-file GC. Drops entry_sources + entries + entries_vec (no
    // cascade) + file_hashes + edges for files confirmed absent from disk. STRICTLY
    // scoped to THIS idx: sources are repo-RELATIVE and therefore NOT unique across
    // repos (two repos can both have "src/a.ts"), so every delete must be constrained
    // to this idx's entries — a bare `WHERE source=?` would cross-delete sibling repos.
    if (root.empty() || idx.empty()) return 0;
    namespace fs = std::filesystem;

    // Candidates: (source, toplevel) pairs this idx's entries are visible at. Absolute
    // path = toplevel + "/" + source (toplevel is the checkout root; for a plain scan
    // without entry_sources, fall back to `root`). Only this idx's rows — no global
    // file_hashes scan (its relative keys collide across repos).
    struct Cand { std::string source, toplevel; };
    std::vector<Cand> candidates;
    {
        Db::Stmt q(db_,
            "SELECT DISTINCT e.source, COALESCE(es.toplevel, ?) "
            "FROM entries e LEFT JOIN entry_sources es ON es.entry_id = e.id "
            "WHERE e.idx=? AND e.source IS NOT NULL");
        q.bind(1, root);
        q.bind(2, idx);
        while (q.step()) candidates.push_back({q.column_text(0), q.column_text(1)});
    }

    int64_t purged = 0;
    constexpr size_t kCommitBatch = 500;
    size_t since_commit = 0;
    db_.exec("BEGIN");
    // All statements scope to THIS idx (via the entry's idx) so a relative source
    // can't reach a sibling repo's rows.
    Db::Stmt ids_q(db_, "SELECT id FROM entries WHERE idx=? AND source=?");
    Db::Stmt del_vis(db_, "DELETE FROM entry_sources WHERE entry_id=? AND source=?");
    Db::Stmt vis_left(db_, "SELECT 1 FROM entry_sources WHERE entry_id=? LIMIT 1");
    Db::Stmt del_entry(db_, "DELETE FROM entries WHERE id=?");
    Db::Stmt del_vec(db_, "DELETE FROM entries_vec WHERE id=?");
    Db::Stmt del_hash(db_, "DELETE FROM file_hashes WHERE idx=? AND source=?");
    Db::Stmt del_edges(db_, "DELETE FROM edges WHERE idx=? AND file=?");
    for (const auto &c : candidates) {
        // Reconstruct the absolute path at this checkout and stat it. Purge only on
        // CONFIRMED absence (a stat error -> keep, never delete live data).
        std::string base = c.toplevel;
        if (!base.empty() && base.back() != '/') base += '/';
        std::error_code ec;
        if (fs::exists(base + c.source, ec) || ec) continue;
        // Entry ids of THIS idx with this source.
        std::vector<std::string> ids;
        { ids_q.reset(); ids_q.bind(1, idx); ids_q.bind(2, c.source);
          while (ids_q.step()) ids.push_back(ids_q.column_text(0)); }
        for (const auto &id : ids) {
            // Drop this checkout's visibility row; the entry dies only when its LAST
            // entry_sources row is gone (shared content across worktrees survives).
            del_vis.reset(); del_vis.bind(1, id); del_vis.bind(2, c.source); del_vis.step();
            vis_left.reset(); vis_left.bind(1, id);
            if (vis_left.step()) continue;
            del_entry.reset(); del_entry.bind(1, id); del_entry.step();
            del_vec.reset(); del_vec.bind(1, id); del_vec.step();
        }
        // Edges + file-hash for this file, scoped to this idx / this source.
        del_edges.reset(); del_edges.bind(1, idx); del_edges.bind(2, c.source); del_edges.step();
        del_hash.reset(); del_hash.bind(1, idx); del_hash.bind(2, c.source); del_hash.step();
        purged++;
        if (++since_commit >= kCommitBatch) {
            db_.exec("COMMIT"); db_.exec("BEGIN"); since_commit = 0;
        }
    }
    db_.exec("COMMIT");
    return purged;
}

std::vector<Store::FnMetric> Store::top_complexity(int limit, const std::string &filter,
                                                   const std::vector<std::string> &scope_idxs) const {
    // Metrics are inlined columns on entries (Step 5). Rank functions (rows with
    // a non-NULL complexity) by complexity. `name` = the entry's symbol; `file` = its
    // source; `line` = its chunk line.
    // Default excludes vendor/ and node_modules/ paths (amalgamations/deps pollute the
    // ranking); when a filter is provided, it's assumed the caller knows what they want.
    // Also excludes parser artifacts like 'namespace' (C++ anonymous namespace blocks).
    std::vector<FnMetric> out;
    std::string sql =
        "SELECT source,symbol,complexity,lines,params,returns,max_depth,line "
        "FROM entries WHERE complexity IS NOT NULL"
        " AND symbol != 'namespace'";  // C++ anonymous namespace blocks aren't functions
    if (!scope_idxs.empty()) {           // scope to one repo (CWD) or a union (parent dir)
        sql += " AND idx IN (";
        for (size_t j = 0; j < scope_idxs.size(); j++) sql += (j ? ",?" : "?");
        sql += ")";
    }
    if (!filter.empty()) {
        sql += " AND source LIKE ?";
    } else {
        // Default exclusions: vendor directories that pollute complexity rankings
        sql += " AND source NOT LIKE '%/vendor/%'"
               " AND source NOT LIKE '%/node_modules/%'"
               " AND source NOT LIKE '%/third_party/%'"
               " AND source NOT LIKE '%/external/%'";
    }
    sql += " ORDER BY complexity DESC, lines DESC LIMIT ?";
    Db::Stmt s(db_, sql);
    int i = 1;
    for (const auto &idx : scope_idxs) s.bind(i++, idx);
    if (!filter.empty()) s.bind(i++, "%" + filter + "%");
    s.bind(i, (int64_t)limit);
    while (s.step()) {
        FnMetric m;
        m.file = s.column_text(0); m.name = s.column_text(1);
        m.complexity = (int)s.column_int64(2); m.lines = (int)s.column_int64(3);
        m.params = (int)s.column_int64(4); m.returns = (int)s.column_int64(5);
        m.max_depth = (int)s.column_int64(6); m.line = (int)s.column_int64(7);
        out.push_back(std::move(m));
    }
    return out;
}

Store::EntryMetric Store::metric_for_entry(const std::string &id) const {
    EntryMetric out;
    // Metrics are inlined ON the entry row (Step 5): the chunk's OWN function metric,
    // attached at chunk time by exact line match. So this is a direct read — no fuzzy
    // (source,line)-range post-join (which mis-attributed ~46% of the time). A non-NULL
    // complexity => is_exact (the metric belongs to THIS chunk's function).
    Db::Stmt q(db_,
        "SELECT source,symbol,complexity,lines,params,returns,max_depth,line "
        "FROM entries WHERE id=?");
    q.bind(1, id);
    if (!q.step()) return out;
    std::string src = q.column_text(0);
    if (src.empty()) return out;
    // complexity NULL => this chunk holds no function metric.
    if (sqlite3_column_type(q.raw(), 2) == SQLITE_NULL) return out;
    out.found = true; out.is_exact = true;
    out.m.file = src; out.m.name = q.column_text(1);
    out.m.complexity = (int)q.column_int64(2); out.m.lines = (int)q.column_int64(3);
    out.m.params = (int)q.column_int64(4); out.m.returns = (int)q.column_int64(5);
    out.m.max_depth = (int)q.column_int64(6); out.m.line = (int)q.column_int64(7);
    return out;
}

Store::LinkCounts Store::link_counts(const std::string &symbol) const {
    LinkCounts c;
    if (symbol.empty()) return c;
    { Db::Stmt s(db_, "SELECT COUNT(*) FROM edges WHERE kind='call' AND src_sym=?");
      s.bind(1, symbol); if (s.step()) c.callees = (int)s.column_int64(0); }
    { Db::Stmt s(db_, "SELECT COUNT(*) FROM edges WHERE kind='call' AND dst_sym=?");
      s.bind(1, symbol); if (s.step()) c.callers = (int)s.column_int64(0); }
    return c;
}

std::vector<Store::Link> Store::links(const std::string &node, const std::string &kind,
                                            const std::string &direction, int depth,
                                            const gogol::ResolvedIdx &index_r) const {
    const std::string &index = index_r.value();  // ResolvedIdx: resolved by construction
    std::vector<Link> out;
    if (depth < 1) depth = 1;
    auto one_hop = [&](const std::string &n, std::vector<Link> &acc) {
        auto run = [&](bool node_is_src) {
            std::string sql = "SELECT kind,src_sym,dst_sym,idx,file FROM edges WHERE ";
            sql += node_is_src ? "src_sym=?" : "dst_sym=?";
            if (!kind.empty()) sql += " AND kind=?";
            if (!index.empty()) sql += " AND idx=?";   // scope out cross-index name collisions
            Db::Stmt s(db_, sql);
            int b = 1;
            s.bind(b++, n);
            if (!kind.empty()) s.bind(b++, kind);
            if (!index.empty()) s.bind(b++, index);
            while (s.step())
                acc.push_back({s.column_text(0), s.column_text(1), s.column_text(2),
                               s.column_text(3), s.column_text(4)});
        };
        if (direction == "out" || direction == "both") run(true);
        if (direction == "in"  || direction == "both") run(false);
    };
    // BFS out to `depth` hops; visited-set makes it cycle-safe. The "next" node of
    // an edge is the end opposite the one we matched on (dst for out, src for in).
    std::set<std::string> visited{node};
    std::vector<std::string> frontier{node};
    std::set<std::string> emitted; // dedup identical edges across hops
    for (int d = 0; d < depth && !frontier.empty(); ++d) {
        std::vector<std::string> next;
        for (const auto &n : frontier) {
            std::vector<Link> hop;
            one_hop(n, hop);
            for (auto &e : hop) {
                std::string key = e.kind + "\x1f" + e.src + "\x1f" + e.dst;
                if (emitted.insert(key).second) out.push_back(e);
                // enqueue the far end for the next hop
                const std::string &far = (e.src == n) ? e.dst : e.src;
                if (visited.insert(far).second) next.push_back(far);
            }
        }
        frontier.swap(next);
    }
    return out;
}

std::vector<std::string> Store::neighbour_ids(const std::string &sym,
                                                 const std::string &kind) const {
    std::vector<std::string> out;
    // dst symbols called-from `sym`, resolved to entry ids via the entries.symbol
    // column (Step 4: was a JOIN symbols; now the symbol lives on the entry row).
    Db::Stmt s(db_,
        "SELECT DISTINCT ce.id FROM edges e "
        "JOIN entries ce ON ce.symbol = e.dst_sym "
        "WHERE e.kind=? AND e.src_sym=?");
    s.bind(1, kind); s.bind(2, sym);
    while (s.step()) out.push_back(s.column_text(0));
    return out;
}

std::vector<std::string> Store::ids_for_symbol(const std::string &symbol) const {
    std::vector<std::string> out;
    if (symbol.empty()) return out;
    Db::Stmt s(db_, "SELECT id FROM entries WHERE symbol=?");
    s.bind(1, symbol);
    while (s.step()) out.push_back(s.column_text(0));
    return out;
}

std::vector<Store::KwHit> Store::keyword_search_scored(const std::string &query_text, int k,
                                                        const std::vector<std::string> *restrict_ids) const {
    std::vector<KwHit> out;
    if (k <= 0) return out;
    const std::string match = build_fts_match(query_text);
    if (match.empty()) return out;
    std::optional<std::set<std::string>> allow;
    if (restrict_ids) allow.emplace(restrict_ids->begin(), restrict_ids->end());
    // Pull FTS5's bm25 rank alongside the id (f.rank, more negative = better).
    Db::Stmt q(db_,
        "SELECT ce.id, f.rank FROM entries_fts f "
        "JOIN entries ce ON ce.rowid = f.rowid "
        "WHERE entries_fts MATCH ? ORDER BY f.rank LIMIT ?");
    q.bind(1, match);
    q.bind(2, (int64_t)(k * 4)); // over-fetch; filtering may drop some
    while (q.step() && (int)out.size() < k) {
        std::string id = q.column_text(0);
        if (allow && !allow->count(id)) continue;
        out.push_back({std::move(id), q.column_double(1)});
    }
    return out;
}

std::vector<Store::GrepHit> Store::literal_search(const std::string &phrase, int k,
                                                   const gogol::ResolvedIdx &index_r) const {
    const std::string &index = index_r.value();  // ResolvedIdx: resolved by construction
    std::vector<GrepHit> out;
    if (phrase.empty() || k <= 0) return out;
    // FTS5 phrase search: wrap in double quotes for exact match (word boundaries
    // still apply, but no stemming/OR-splitting). Escape internal quotes.
    std::string escaped;
    for (char c : phrase) {
        if (c == '"') escaped += "\"\"";  // FTS5 escapes " by doubling
        else escaped += c;
    }
    std::string match = "\"" + escaped + "\"";
    
    // Optional index filter
    std::string sql =
        "SELECT ce.id, ce.source, ce.line, ce.symbol FROM entries_fts f "
        "JOIN entries ce ON ce.rowid = f.rowid "
        "WHERE entries_fts MATCH ?";
    if (!index.empty()) sql += " AND ce.idx = ?";
    sql += " ORDER BY f.rank LIMIT ?";
    
    Db::Stmt q(db_, sql);
    int i = 1;
    q.bind(i++, match);
    if (!index.empty()) q.bind(i++, index);
    q.bind(i, (int64_t)k);
    
    while (q.step()) {
        GrepHit h;
        h.id = q.column_text(0);
        h.source = q.column_text(1);
        h.line = (int)q.column_int64(2);
        h.symbol = q.column_text(3);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Store::HybridHit> Store::query_hybrid(const std::vector<float> &qvec, int k,
                                                        const gogol::ResolvedIdx &index_r,
                                                        const std::string &type,
                                                        const std::string &scope_path,
                                                        const std::string &query_text,
                                                        const std::string &source_prefix,
                                                        const std::vector<std::string> &restrict_refs) const {
    const std::string &index = index_r.value();  // ResolvedIdx: resolved by construction
    // index scoping: restrict to entries tagged index:<name> (empty = all indexes).
    // A single `index` and a multi-ref `restrict_refs` (parent-dir search, T3) both
    // feed the same filter; union them (ids_in_indexes matches idx OR root_sha).
    std::vector<std::string> restrict_all;
    if (!index.empty()) restrict_all.push_back(index);
    for (const auto &r : restrict_refs) if (!r.empty()) restrict_all.push_back(r);

    // Compute the index-filtered candidate ids ONCE and reuse for both arms.
    // ids_in_indexes does a SQL query + N string copies; for a large index (godot-
    // docs: 11314 ids) it's ~38ms, and calling it twice (semantic arm + keyword
    // arm) was ~76ms of redundant work. Precompute when there's an index restrict.
    std::vector<std::string> index_ids;
    const std::vector<std::string> *precomp = nullptr;
    if (!restrict_all.empty()) {
        index_ids = ids_in_indexes(restrict_all);
        precomp = &index_ids;
    }

    // --- ARM 1: semantic cosine top-k (scoped by index+type+scope). ---
    auto sem = query(qvec, k, restrict_all, {}, type, scope_path, precomp);

    // --- ARM 2: FTS5 keyword top-k over the same candidate set. Notes live in
    // notes.db (no FTS there), so the keyword arm is code/doc only — which is where
    // identifier/keyword matches matter. Skip keyword arm entirely for type=note.
    // Scored variant: we keep each hit's bm25 rank so the A3 blend can use magnitude,
    // not just position.
    std::vector<KwHit> kw;
    bool want_keywords = type.empty() || type == "code" || type == "doc";
    if (!query_text.empty() && want_keywords) {
        std::vector<std::string> cand;
        bool have_restrict = !restrict_all.empty();
        bool typed = (type == "code" || type == "doc");
        if (have_restrict || typed) {
            if (have_restrict) cand = index_ids;  // reuse the precomputed set
            if (typed) {
                auto tids = ids_of_type(type);
                if (have_restrict) {
                    std::set<std::string> ts(tids.begin(), tids.end());
                    std::vector<std::string> inter;
                    for (auto &id : cand) if (ts.count(id)) inter.push_back(id);
                    cand.swap(inter);
                } else cand.swap(tids);
            }
            kw = keyword_search_scored(query_text, k, &cand);
        } else {
            kw = keyword_search_scored(query_text, k, nullptr);
        }
    }

    // --- FUSE via weighted blend of normalized signals ("A3"). Replaces plain RRF.
    // Rationale (measured, 8-trial known-item eval): when cosine scores cluster
    // tightly (avg top-10 spread ~0.077), rank-only RRF discards the magnitude that
    // separates near-ties. The blend keeps both signals' magnitudes:
    //
    //   score = W_SEM * cosine  +  W_KW * bm25_norm
    //
    // cosine is already 0..1. bm25 (FTS5 rank, more-negative = better) is min-max
    // normalized to 0..1 WITHIN this result set. A hit present in only one arm scores
    // 0 for the missing arm (so pure-semantic and pure-keyword hits still rank by their
    // available signal). This improved recall@1 by ~19pts and MRR by ~0.10 over RRF.
    constexpr double W_SEM = 0.6;
    constexpr double W_KW  = 0.4;

    std::unordered_map<std::string, double> blended;
    std::unordered_map<std::string, float> cosine_map; // id → raw cosine similarity
    std::vector<std::string> order; // first-seen order for stable output
    auto ensure = [&](const std::string &id) {
        if (blended.find(id) == blended.end()) { blended[id] = 0.0; order.push_back(id); }
    };

    // Semantic arm: cosine is already in [0,1]; contribute W_SEM * cosine.
    for (const auto &h : sem) {
        ensure(h.id);
        cosine_map[h.id] = h.score;              // raw cosine (1 - distance)
        blended[h.id] += W_SEM * static_cast<double>(h.score);
    }

    // Keyword arm: min-max normalize bm25 across the kw hits, then contribute
    // W_KW * bm25_norm. FTS5 rank is more-negative = better, so the best hit maps to
    // 1.0 and the worst to 0.0. Degenerate case (all equal / single hit) => all 1.0.
    if (!kw.empty()) {
        double bm_min = kw.front().bm25, bm_max = kw.front().bm25;
        for (const auto &h : kw) {
            if (h.bm25 < bm_min) bm_min = h.bm25;
            if (h.bm25 > bm_max) bm_max = h.bm25;
        }
        const double range = bm_max - bm_min;
        for (const auto &h : kw) {
            // more-negative bm25 = better → (bm_max - bm25) puts best at the top.
            double norm = (range > 1e-12) ? (bm_max - h.bm25) / range : 1.0;
            ensure(h.id);
            blended[h.id] += W_KW * norm;
        }
    }

    // Sort by blended score desc; deterministic id tiebreak for stable output.
    std::sort(order.begin(), order.end(), [&](const std::string &a, const std::string &b) {
        if (blended[a] != blended[b]) return blended[a] > blended[b];
        return a < b;
    });

    // --- source_prefix filtering (toplevel scoping): restrict to entries that exist
    // in the given checkout (toplevel). With the entry_sources table, an entry is
    // visible if there's a row where entry_sources.toplevel = source_prefix.
    //
    // This enables worktree support: same content (same entry.id) can appear in main
    // AND a worktree — each gets its own entry_sources row. Querying from the worktree
    // (source_prefix=/path/to/worktree) returns entries visible there.
    //
    // When both index and source_prefix are set: index scopes to a repo (common_dir),
    // source_prefix further restricts to a specific checkout. Both filters apply. ---
    auto source_matches = [&](const std::string &id) -> bool {
        if (source_prefix.empty()) return true;
        // Notes live in notes.db, not cache.db — they have no entry_sources row.
        // Their scoping is handled by scope_path (path-prefix) in query(), not by
        // toplevel visibility. So a note always passes the toplevel filter.
        if (notes_db_) {
            Db::Stmt nck(*notes_db_, "SELECT 1 FROM notes WHERE id=? LIMIT 1");
            nck.bind(1, id);
            if (nck.step()) return true;  // it's a note: bypass entry_sources
        }
        // Check if there's an entry_sources row for this (id, toplevel).
        Db::Stmt s(db_, "SELECT 1 FROM entry_sources WHERE entry_id=? AND toplevel=? LIMIT 1");
        s.bind(1, id);
        s.bind(2, source_prefix);
        return s.step();  // true if a row exists
    };

    std::vector<HybridHit> out;
    std::unordered_set<std::string> seen;
    for (const auto &id : order) {
        if (!source_matches(id)) continue;
        float cosine = 0.0f;
        auto cit = cosine_map.find(id);
        if (cit != cosine_map.end()) cosine = cit->second;
        out.push_back({id, static_cast<float>(blended[id]), false, cosine});
        seen.insert(id);
    }

    // --- Call-graph expansion of the fused hits (structure the coordinates miss),
    // BOUNDED: cap edge hits to ~k total and skip high-fan-out hubs. Rank stays
    // fusion-first via the 0.5x discount on edge neighbours. ---
    int edge_budget = k;
    std::vector<HybridHit> core(out.begin(), out.end());
    for (auto &h : core) {
        if (edge_budget <= 0) break;
        auto e = get(h.id);
        if (!e) continue;
        std::string sym = leading_symbol(e->content);
        if (sym.empty()) continue;
        auto nb = neighbour_ids(sym, "call");
        if (nb.size() > 12) continue; // skip explosive hubs (generic callers)
        for (auto &nid : nb) {
            if (edge_budget <= 0) break;
            if (seen.count(nid)) continue;
            if (!source_matches(nid)) continue;  // also apply source_prefix to neighbours
            if (!get(nid)) continue; // orphaned id (stale) — skip
            seen.insert(nid);
            // Edge hits get 0 cosine (not in semantic results — only reached via graph)
            out.push_back({nid, h.score * 0.5f, true, 0.0f});
            edge_budget--;
        }
    }
    return out;
}


} // namespace store
