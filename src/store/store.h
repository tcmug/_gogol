// store.h — gogol "memory" store (the store; v1 indexes retired, only the
// (v1 is fully retired; the one-time v1->v2 cutover bridge has been removed).
//
// ONE unified store. Every citizen — code chunk, doc, AND note — is an `entry`
// with an immutable content-hash identity, coordinates (nomic), and index
// membership (the `idx` column; worktree visibility is the `entry_sources` junction).
//
// Design invariants (validated empirically — see experiments/mem-v2/ and docs/DESIGN.md):
//   * identity (id = content hash) != address (derived) != membership. Id never changes.
//   * an entry carries a primary `idx`; worktree visibility is a junction
//     (`entry_sources`). Cross-index search is the default; scoping filters by index.
//
// This does NOT implement StorageBackend (v1's per-index IndexData shape is
// deliberately different). It is a standalone store built on the Db primitive.
// Mirrors experiments/mem-v2/memstore.py 1:1.
#pragma once
#include "storage/db.h"
#include "store/resolved_idx.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

namespace store {

struct Entry {
    std::string id;
    std::string content;
    std::vector<float> embedding; // L2-normalized, dim = kDim
    std::string source;
    std::string symbol;           // function/class name (empty for docs without heading)
    int line = 0, end_line = 0;   // 1-based chunk location within source (0 = unknown)
    std::vector<std::string> indexes; // bare index names this entry belongs to (the `idx` column)
    std::string type; // derived from source (not stored) — note|doc|code
    std::string supersedes;      // note: id this note replaced (forward link)
    std::string superseded_by;   // note: id that replaced this note (reverse link)
    std::string resolved_from;   // set by get_latest(): the requested id, when it was superseded
};

struct Hit {
    std::string id;
    float score; // cosine
};

// nomic-embed-text-v1.5 native dimension.
constexpr int kDim = 768;

// Content-addressed identity: BLAKE2b(NUL content), first 16 hex chars.
std::string content_id(const std::string &content);

// Leading identifier of a chunk = its local symbol name (matches v1 edge symbol
// tails). "handler(...)" -> "handler"; "Foo.get(...)" -> "get". Exposed so the
// migration can map entry symbols and query-time expansion can resolve them.
std::string extract_leading_symbol(const std::string &content);

// Extract the bare symbol name from a tree-sitter chunk HEADING (AdapterChunk.heading),
// which is decorated as "Parent.name(params): ret". Strips the "(...)" signature and
// return type, then symbol_tail() strips any "Parent."/"Parent::" qualifier — yielding
// the function name that call-graph edges are keyed on. Prefer this over
// extract_leading_symbol(enriched_text): the enriched embed text is prefixed with
// directory-context tokens, so extract_leading_symbol would return a dir name (e.g.
// "storage"/"core") instead of the symbol.
std::string symbol_from_heading(const std::string &heading);

// Tail of a qualified symbol: strips both "." and "::" separators, so
// "Foo.bar", "Foo::bar", and "a::b::c" all -> the final segment. Used to key
// call-graph edges consistently across TS (dot) and C++ (::).
std::string symbol_tail(const std::string &sym);

// Derive entry type from its source (the single source of truth for type):
//   empty/NULL       -> "note"  (LLM-authored; no external source)
//   "file://" .md    -> "doc"
//   "file://" other  -> "code"
//   "classifier://"  -> "classifier"
// No stored type column/tag — type is always this projection of `source`.
std::string type_from_source(const std::string &source);

// Stage 3 SPLIT: Store spans TWO SQLite files by durability class.
//   * cache.db (the `path` arg)  — derived/re-derivable: code+doc entries +
//     embeddings, graph edges, metrics, FTS. Any bulk/rebuild op only touches this.
//   * notes.db (the `notes_path` arg) — the note CONTENT of record + scope_path +
//     its embedding. NOT re-derivable from disk; kept OUT of cache.db so a cache
//     rebuild never destroys notes. Gets only add-one / forget-one-by-id.
// A note (source empty) is written to notes.db; a doc/code entry (non-empty source)
// to cache.db. query() unions both by cosine in the same vector space, so one query
// still returns code + notes together. Notes are scope-filtered at query time
// (path-prefix surfacing). When notes_path is empty the store is cache-only (notes
// ops become no-ops) — used by tests/tools that never touch notes.
class Store {
public:
    explicit Store(const std::string &path); // single-arg: derives a sibling notes.db (tests/one-shot opens)
    Store(const std::string &path, const std::string &notes_path); // explicit notes path ("" => cache-only)
    // Read-only open: SQLITE_OPEN_READONLY, no schema DDL (readers never migrate).
    // Use for query/status so they never contend with a concurrent sync's write lock.
    struct ReadOnly {};
    Store(const std::string &path, ReadOnly);
    Store(const std::string &path, const std::string &notes_path, ReadOnly);

    // Insert a DERIVED entry (code/doc) into cache.db. Identity = content hash
    // (identical content => same id => automatic dedup). Returns the id. `embedding`
    // is L2-normalized on write. A note (source empty) is rejected here — use
    // put_note(); notes live in notes.db, not cache.db.
    // `toplevel` is the git checkout path this file was indexed from (e.g.
    // "/path/to/main" or "/path/to/worktree"); recorded in entry_sources for
    // per-checkout scoping at query time.
    std::string put(const std::string &content, const std::vector<float> &embedding,
                    const std::vector<std::string> &indexes = {},
                    const std::string &source = "",
                    const std::string &toplevel = "");

    // Insert a NOTE (LLM-authored, no external source) into notes.db with an optional
    // path scope. Identity = content hash (dedup). scope_path empty => a global note
    // (surfaces for every query); a non-empty scope surfaces only when the query scope
    // is that path or a descendant of it (path-prefix surfacing). Returns the id (empty
    // if the store has no notes db, e.g. a cache-only test store).
    std::string put_note(const std::string &content, const std::vector<float> &embedding,
                         const std::string &scope_path = "",
                         const std::string &supersedes = "");

    // Replace a note atomically by archiving the old row in-place: the old note is
    // kept in notes.db (for get() history), but its notes_vec row is deleted so it
    // stops surfacing in recall. The new note is inserted with a backward link to the
    // superseded note (`supersedes=old_id`). If `old_id` doesn't exist or is a
    // code/doc entry, the archive is best-effort (refusal is NOT fatal — the new
    // note is still put). Returns the new note id; sets `forgotten` to true iff an
    // existing note was moved out of the live search set, `archived` to true iff the
    // old note was kept as history, and `refused` to true iff old_id was a code/doc
    // entry. Empty id if the store has no notes db.
    struct ConsolidateResult {
        std::string id;        // the new note's id (content hash of `content`)
        bool forgotten = false;  // existing note stopped surfacing in recall
        bool archived = false;    // old note kept in notes.db as history
        bool refused = false;     // old_id was a code/doc entry (archive refused, put still happened)
    };
    ConsolidateResult consolidate_note(const std::string &old_id,
                                        const std::string &content,
                                        const std::vector<float> &embedding,
                                        const std::string &scope_path = "");

    // Per-chunk location + inlined per-function metrics, stamped at ingest (all
    // optional; notes/docs omit metrics). has_metric distinguishes "no function in
    // this chunk" (leave metric columns NULL) from a genuine zero-complexity function.
    struct Loc {
        int line = 0, end_line = 0;
        std::string symbol;
        bool has_metric = false;
        int complexity = 0, lines = 0, params = 0, returns = 0, max_depth = 0;
    };
    std::string put(const std::string &content, const std::vector<float> &embedding,
                    const std::vector<std::string> &indexes, const std::string &source,
                    const Loc &loc, const std::string &toplevel = "",
                    const std::string &root_sha = "");

    // Record that entry `id` belongs to index `index_name` (bare name, e.g.
    // "backend"). Typed membership row in entry_index; many-to-many (deduped content
    // can belong to >1 index). Replaces the old free-text add_tag("index:X").
    void add_membership(const std::string &id, const std::string &index_name);

    // Cheap existence check by content id (no embedding load) — lets ingest skip the
    // expensive embed for content already stored (content-hash identity => same id =>
    // identical vector). Returns true if an entry with this id exists.
    [[nodiscard]] bool has_entry(const std::string &id) const;

    // Non-blocking probe: is another process currently holding the write lock?
    // Attempts BEGIN IMMEDIATE with a 0ms busy_timeout; if it returns SQLITE_BUSY a
    // writer (e.g. a concurrent refresh/sync) owns the lock. Immediately rolls back
    // on success so this never actually writes. Lets a reader skip the
    // halt-until-indexed refresh and query stale-but-available data instead of
    // blocking. Returns true if busy (writer active), false if the lock is free.
    [[nodiscard]] bool writer_busy();

    // Refresh membership + location for an ALREADY-stored entry WITHOUT re-embedding
    // (the embed-skip path). Use when has_entry(id) is true so an unchanged chunk
    // costs zero embed on resync. Also records the toplevel in entry_sources.
    void update_entry_loc(const std::string &id, const std::string &index_name, const Loc &loc,
                     const std::string &toplevel = "");

    // Per-file stat-hash for the sync skip gate, keyed by (idx, source) — relative
    // sources aren't unique across repos. file_hashes(idx) loads THIS idx's
    // (source -> hash) map for the scanner; set_file_hash records one after ingest.
    std::map<std::string, uint64_t> file_hashes(const std::string &idx) const;
    void set_file_hash(const std::string &idx, const std::string &source, uint64_t hash);
    void set_file_hashes_batch(const std::string &idx,
                               const std::map<std::string, uint64_t> &hashes);

    // Entry ids of a given type (note|doc|code), derived from source. Used to
    // PRE-filter query candidates by type so note/doc search restricts the search
    // space (not a post-filter on top-k, which starves rare types like notes).
    std::vector<std::string> ids_of_type(const std::string &type) const;

    // Bare index names entry `id` belongs to (from entry_index). Populates Entry.indexes.
    std::vector<std::string> indexes_for(const std::string &id) const;

    // Ids belonging to ANY of the given indexes (names may be bare or "index:"-
    // prefixed; the prefix is stripped). Empty => all entries.
    std::vector<std::string> ids_in_indexes(const std::vector<std::string> &names) const;

    // An index paired with a checkout PATH to match a user scope name/suffix against.
    // Since entries.idx is the root SHA, repo_tag(idx)/suffix-of(idx) can't match a
    // repo NAME anymore — the name lives in the checkout path, not the SHA. This pairs
    // each idx with its most-recent ref_sightings path so the scope resolver matches
    // a bare name/suffix against the PATH and returns the SHA idx. `path` falls back
    // to the idx itself when there is no sighting (legacy path-keyed indexes), so the
    // old behavior still works. (gogol::IndexLoc lives in resolved_idx.h — shared with
    // the resolver so no conversion is needed at the call site.)
    std::vector<gogol::IndexLoc> index_locations() const;

    // Human-readable display path for an index (T9). entries.idx is a root-commit SHA
    // now, which is cryptic in output. Return a representative checkout path for the
    // idx (any entry_sources.toplevel under it); if none (or idx is already a path
    // from the no-SHA fallback), return idx unchanged. Pure display — never an identity.
    std::string index_display_path(const std::string &idx) const;

    // Reconstruct a display (absolute) path for a stored REPO-RELATIVE source: the
    // index's live checkout path + "/" + rel. Returns `rel` unchanged when it can't
    // resolve a checkout (no sighting/entry_sources), or when `rel` is empty (a note),
    // or already absolute (legacy/unmigrated row). One place so every surface that
    // emits `source` reconstructs it the same way.
    std::string display_source(const std::string &idx, const std::string &rel) const;

    // --- ref_sightings: the location-layer helper (root_sha -> on-disk paths). ---
    struct Sighting {
        std::string root_sha;
        std::string path;
        int64_t last_seen = 0;
        bool overlay = false;
    };
    // Best-effort upsert of a (root_sha, path) sighting with now() as last_seen.
    // Preserves an existing overlay flag (COALESCE). Swallows errors (e.g. write-lock
    // contention) so a read command is never failed/blocked by learning a location.
    void record_sighting(const std::string &root_sha, const std::string &path);
    // All sightings for a ref (any path it has been seen at), most-recent first.
    std::vector<Sighting> sightings_for(const std::string &root_sha) const;
    // Distinct refs whose sighted path is under `dir` (prefix match on a path
    // boundary), paired with that sighted path so the caller can stat it without a
    // re-query. Used by parent-dir multi-repo search — caller stat-filters to live.
    std::vector<Sighting> sightings_under(const std::string &dir) const;
    // Set/clear the overlay flag on a sighting (inserts the row if absent). Returns
    // false if root_sha/path empty.
    bool set_overlay(const std::string &root_sha, const std::string &path, bool on);
    // Refs currently flagged as overlays (for union-ing into recall).
    std::vector<Sighting> active_overlays() const;

    // Build the multi-ref restriction set for recall (shared by CLI + MCP, R4/R7):
    //  - adds refs SIGHTED under `dir` (parent-dir multi-repo search) when `dir` is a
    //    non-empty directory scope, and
    //  - unions active-overlay refs, but ONLY when the result is already a restriction
    //    (either `base_scoped` is true — a single index/repo resolved — or the
    //    parent-dir search produced refs). Under a fully global search overlays are
    //    NOT added, since turning an empty restriction into a ref list would flip a
    //    global search into an overlay-only one.
    // Every candidate is stat-filtered to a live on-disk path. Deduped by ref. One
    // place so CLI and MCP can't drift.
    std::vector<std::string> recall_restrict_refs(const std::string &dir,
                                                  bool base_scoped) const;

    [[nodiscard]] std::optional<Entry> get(const std::string &id) const;
    // get() that follows superseded_by to the newest version of a note. Sets
    // resolved_from when it moved. Same as get() for non-notes / unsuperseded notes.
    [[nodiscard]] std::optional<Entry> get_latest(const std::string &id) const;

    // Cross-index by DEFAULT. Scope by passing tag filters. Ranks by cosine.
    // `scope_path` (a directory path, e.g. a repo toplevel or a subdir) controls
    // NOTE surfacing: a note whose scope_path is a prefix of `scope_path` (or is
    // empty/global) is included; notes scoped elsewhere are excluded. Empty
    // `scope_path` => only global (unscoped) notes surface. Code/doc entries are
    // unaffected by scope_path (they're filtered by index/type only).
    [[nodiscard]] std::vector<Hit> query(const std::vector<float> &qvec, int k = 10,
                           const std::vector<std::string> &restrict_all = {},
                           const std::vector<std::string> &restrict_any = {},
                           const std::string &type = "",
                           const std::string &scope_path = "",
                           const std::vector<std::string> *precomputed_ids = nullptr) const;

    // True iff a note should surface for a query, under the three-shape note model.
    // A note carries (note_scope_id, note_scope_sub):
    //   - GLOBAL: both empty -> always surfaces.
    //   - REPO-scoped: note_scope_id = repo ref, note_scope_sub = relative subpath
    //     (""=whole repo). Surfaces iff query_ref == note_scope_id AND the query's
    //     subpath is at/under note_scope_sub.
    //   - NON-REPO: note_scope_id empty, note_scope_sub = an ABSOLUTE path. Surfaces
    //     iff the query's absolute path is at/under it (path-prefix).
    // The query is resolved ONCE to (query_ref, query_sub, query_abs) — the same
    // resolution code uses. Static + pure so it's unit-testable without a DB.
    static bool note_scope_matches(const std::string &note_scope_id,
                                   const std::string &note_scope_sub,
                                   const std::string &query_ref,
                                   const std::string &query_sub,
                                   const std::string &query_abs);

    // Forget a NOTE by id (source IS NULL — LLM-authored). Refuses to remove
    // disk-sourced entries (those are governed by disk+sync, not the agent).
    // Returns: 1 removed, 0 not-found, -1 refused (not a note).
    int forget_note(const std::string &id);

    // --- disk reconciliation (staleness) ---
    // Remove every entry whose source == `source` (all chunks of one file).
    // Used on file change (remove-then-reingest) and file delete.
    int64_t remove_by_source(const std::string &source);
    // All distinct non-NULL sources currently in the store (= files represented).

    // Fetch an entry's embedding by id (from vec0 index). Returns empty if not found.
    [[nodiscard]] std::vector<float> get_embedding(const std::string &id) const;

    // Find entries related to a given entry (by embedding similarity).
    // Takes an entry id, retrieves its embedding, and runs cosine KNN.
    // Optional type filter: "code", "doc", or empty for all (excluding notes).
    [[nodiscard]] std::vector<Hit> related(const std::string &id, int k = 10,
                                           const std::string &type = "") const;

    // --- structural graph (edges by symbol + symbol->id map) ---
    void add_edge(const std::string &kind, const std::string &src_sym,
                  const std::string &dst_sym, const std::string &idx,
                  const std::string &file = "");
    // Remove all edges extracted from `file` (used before re-extracting a changed
    // file, so the graph doesn't accumulate stale edges). Returns count removed.
    int64_t remove_edges_of_file(const std::string &file);

    // Remove entries of `source` whose id is NOT in keep_ids (the file's CURRENT
    // chunk set). Purges old-content chunks of a changed file WITHOUT touching the
    // unchanged ones (which keep their original `created` — the last-changed signal).
    // Fixes per-chunk staleness: sync previously only removed entries of DELETED
    // files, so changed files accumulated stale old-content chunks.
    int64_t remove_stale_chunks(const std::string &idx, const std::string &source,
                                const std::vector<std::string> &keep_ids);

    // GC for DELETED files: purge every stored trace of sources under `root`
    // (prefix-guarded) that no longer exist on disk. Candidates = file_hashes
    // keys UNION DISTINCT entries.source of `idx`, detection is fs::exists with
    // error_code — a source is purged only on CONFIRMED absence (a stat error or
    // a path outside `root` is skipped, never purged). Per gone source: drop its
    // entry_sources rows, drop entries left with zero sources (plus their
    // entries_vec rows), its edges, its file_hashes row. Returns count of sources
    // purged. Safe after any resync, including a budgeted one: detection is
    // disk-state-based, not scan-progress-based.
    int64_t purge_missing_sources(const std::string &idx, const std::string &root);

    // --- function complexity metrics (tree-sitter; a real quality signal, unlike
    // embeddings). Step 5: INLINED as columns on entries, attached to the chunk
    // row at chunk time by exact line match — no separate table, no fuzzy join. ---
    struct FnMetric {
        std::string file, name;
        int complexity = 0, lines = 0, params = 0, returns = 0, max_depth = 0, line = 0;
    };
    // Highest-complexity functions (optional path substring filter + optional idx
    // scope). The refactor-candidate / quality view. Reads entries WHERE complexity
    // NOT NULL. `scope_idxs`: the set of idx values (repo refs) to rank within — ONE
    // for a single repo, SEVERAL for a parent-dir union, EMPTY = all indexes. The
    // caller decides; metrics never silently ranks the whole store when a scope was
    // intended (an unresolved scope passes an empty-but-sentinel set — see the CLI).
    std::vector<FnMetric> top_complexity(int limit = 20, const std::string &filter = "",
                                         const std::vector<std::string> &scope_idxs = {}) const;

    // Complexity signal for a code entry. Step 5: metrics are INLINED on the entry
    // row (attached to THIS chunk's own function at chunk time by exact line match),
    // so a found metric is always `is_exact` — a direct read, no fuzzy fallback. Not
    // found (found=false) => the chunk holds no function (doc/markdown, or unmatched).
    // `is_exact` is retained for the caller's honest labelling (fn_complexity).
    struct EntryMetric { FnMetric m; bool found = false; bool is_exact = false; };
    EntryMetric metric_for_entry(const std::string &id) const;
    // entry ids reachable from `sym` via edges (outgoing dst symbols resolved to ids).
    std::vector<std::string> neighbour_ids(const std::string &sym, const std::string &kind = "call") const;
    // entry ids DEFINED by `symbol` (entries.symbol lookup, no graph walk).
    // Used by `delve` to locate a symbol's own defining entry (its source).
    std::vector<std::string> ids_for_symbol(const std::string &symbol) const;

    // General edge query over edges (unifies call/import/docref). Returns raw
    // edges so it works for symbol edges (call/import) AND path edges (docref,
    // whose targets aren't in the symbol map). `kind` empty = any; `direction` is
    // "out" (node is src), "in" (node is dst), or "both".
    struct Link { std::string kind, src, dst, idx, file; };
    std::vector<Link> links(const std::string &node, const std::string &kind = "",
                            const std::string &direction = "both", int depth = 1,
                            const gogol::ResolvedIdx &index = gogol::ResolvedIdx::none()) const;

    // Cheap structural summary for a symbol: {callers, callees} call-edge counts
    // (1-hop). For annotating recall hits inline without returning full edge lists.
    struct LinkCounts { int callers = 0; int callees = 0; };
    LinkCounts link_counts(const std::string &symbol) const;

    // FTS5 keyword search over entries_fts (content + symbol). Returns (id, bm25),
    // best-first (BM25 rank; more-negative = better). `restrict_ids` (optional) limits
    // results to that id set (the same candidate scoping cosine uses); empty => search
    // all. The keyword arm of query_hybrid's A3 weighted blend — the bm25 magnitude is
    // min-max normalized and blended with cosine. Testable in isolation.
    struct KwHit { std::string id; double bm25; };
    [[nodiscard]] std::vector<KwHit> keyword_search_scored(const std::string &query_text, int k,
                                             const std::vector<std::string> *restrict_ids = nullptr) const;

    // Literal phrase search via FTS5. Returns entries containing the exact phrase
    // (no tokenization/stemming beyond FTS5 word boundaries). For known symbols,
    // error messages, config values — deterministic grep-like results.
    struct GrepHit { std::string id; std::string source; int line; std::string symbol; };
    [[nodiscard]] std::vector<GrepHit> literal_search(const std::string &phrase, int k = 50,
                                                       const gogol::ResolvedIdx &index = gogol::ResolvedIdx::none()) const;

    // Hybrid retrieval: semantic cosine top-k BLENDED with an FTS5 keyword arm via a
    // weighted sum (0.6·cosine + 0.4·bm25_norm; the "A3" re-ranker), then each hit
    // expanded to its call/import
    // neighbours (the coordinate+graph complementarity). `query_text` is the raw query
    // string for the FTS5 MATCH arm (empty => cosine-only, back-compat). `source_prefix`
    // scopes to entries whose source path starts with this prefix (toplevel scoping —
    // filters results to a specific checkout/worktree while sharing the same index).
    // Neighbours are appended (deduped) after the fused hits. Returns (id, score, via_edge).
    // `max_cosine` is the raw cosine similarity (0–1) for grounding confidence.
    struct HybridHit { std::string id; float score; bool via_edge; float max_cosine = 0.0f; };
    [[nodiscard]] std::vector<HybridHit> query_hybrid(const std::vector<float> &qvec, int k = 10,
                                        const gogol::ResolvedIdx &index = gogol::ResolvedIdx::none(),
                                        const std::string &type = "",
                                        const std::string &scope_path = "",
                                        const std::string &query_text = "",
                                        const std::string &source_prefix = "",
                                        const std::vector<std::string> &restrict_refs = {}) const;

    // Count entries whose derived type == `type` (note|doc|code). Derived from
    // source; iterates entries (used for stats — not a hot path).
    int64_t count_by_type(const std::string &type) const;

    struct Stats {
        int64_t entries;
        int64_t indexes;
        int64_t edges;
        int64_t vec_entries;  // entries in vec0 ANN index
    };

    // Per-index breakdown for `status`: one row per distinct idx, with its portable
    // root-commit SHA (the ref a container passes as scope) and entry count. This is
    // the one place a connected client can DISCOVER an index's ref — recall results
    // carry the path (idx) but not the SHA. root_sha is empty for a legacy/ambiguous
    // index not yet stamped.
    struct IndexInfo {
        std::string idx;       // the index identity (root SHA, or a path for multi-root/legacy)
        std::string root_sha;  // portable ref (git root commit); "" if none (multi-root/unstamped)
        int64_t entries;       // entries in this index
        // On-disk checkout dir(s) from ref_sightings (may be >1 = worktrees), each with
        // its OWN overlay flag — overlay is a per-checkout property, not per-index (one
        // worktree can be an overlay while the main checkout is not).
        struct Dir { std::string path; bool overlay = false; };
        std::vector<Dir> dirs;
    };
    std::vector<IndexInfo> index_breakdown() const;
    Stats stats() const;

    void begin();   // open an explicit transaction (batched writes; abort-safe)
    void commit();

private:
    // Cached prepared statements for hot-path operations (avoid re-preparing per call).
    // Mutable because they're caches; the logical state doesn't change.
    Db::Stmt& stmt_has_entry() const;
    Db::Stmt& stmt_add_edge() const;
    Db::Stmt& stmt_set_file_hash() const;
    mutable std::unique_ptr<Db::Stmt> stmt_has_entry_;
    mutable std::unique_ptr<Db::Stmt> stmt_add_edge_;
    mutable std::unique_ptr<Db::Stmt> stmt_set_file_hash_;

    mutable Db db_;                        // cache.db (derived: code/doc + graph + FTS)
    std::unique_ptr<Db> notes_db_;         // notes.db (note content of record); null = cache-only
    std::string db_path_;                  // cache.db path (for writer_busy() lock probe)
    bool in_tx_ = false;
};

} // namespace store
