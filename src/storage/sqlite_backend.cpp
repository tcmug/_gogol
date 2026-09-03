// sqlite_backend.cpp — SqliteBackend: one <index>.db per index.
//
// Mechanical mapping of every StorageBackend collection onto the SQLite schema
// (db.cpp / storage-architecture.md). All writes for a given save_* run inside
// one Db::Tx. All params are bound (no string concatenation of values → no SQL
// injection). Query strings that repeat are factored into helper constants.
#include "storage/sqlite_backend.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sqlite3.h>

namespace fs = std::filesystem;

// --- edge kinds (the `kind` discriminator in the unified `edges` table) ------
namespace {
constexpr const char *kKindCall = "call";
constexpr const char *kKindImport = "import";
constexpr const char *kKindDocref = "docref";
constexpr const char *kKindType = "type";

// A tab-free field separator for attrs / symbol lists. Paths, link text and
// symbols never contain a form-feed, so it round-trips losslessly.
constexpr char kSep = '\x1f'; // ASCII unit separator

std::vector<std::string> split_sep(const std::string &s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(kSep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join_sep(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) out += kSep;
        out += v[i];
    }
    return out;
}

// Read a BLOB column (0-based) into a byte string via the raw sqlite3_stmt*.
// The Db::Stmt wrapper deliberately exposes only int64/text readers, so BLOB
// access goes through raw() here.
std::string column_blob(Db::Stmt &st, int col) {
    const void *data = sqlite3_column_blob(st.raw(), col);
    int n = sqlite3_column_bytes(st.raw(), col);
    if (!data || n <= 0) return std::string();
    return std::string(reinterpret_cast<const char *>(data), static_cast<size_t>(n));
}
} // namespace

// --- path -------------------------------------------------------------------

std::string SqliteBackend::db_path(const std::string &index) {
    fs::path dir = fs::path(std::getenv("HOME")) / ".gogol" / "indexes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / (index + ".db")).string();
}

SqliteBackend::SqliteBackend(std::string index) : name_(std::move(index)) {}

Db &SqliteBackend::db() {
    if (!db_) {
        db_ = std::make_unique<Db>(db_path(name_));
        ensure_meta_table();
    }
    return *db_;
}

void SqliteBackend::ensure_meta_table() {
    // Holds Index.dim + Index.precision (the .emb header fields). Kept separate
    // from Db::ensure_schema so the shared schema stays backend-agnostic.
    db_->exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INT)");

    // The design-doc metrics schema omits FunctionMetrics.line, but FileBackend
    // persists it (metrics TSV column 3). Add the column idempotently so metric
    // behavior (used by `gogol metrics`) is preserved 1:1. ALTER TABLE ADD
    // COLUMN throws if it already exists → swallow that specific case.
    try {
        db_->exec("ALTER TABLE metrics ADD COLUMN line INT DEFAULT 0");
    } catch (const std::exception &) {
        // Column already present (re-open) — idempotent, ignore.
    }
}

// ============================================================================
// entries + embeddings  (was .meta + .emb)
// ============================================================================

void SqliteBackend::save_index(const std::string & /*root_path*/, const Index &index) {
    Db &d = db();
    Db::Tx tx(d);
    save_index_locked(index);
    tx.commit();
}

void SqliteBackend::save_index_locked(const Index &index) {
    Db &d = db();

    d.exec("DELETE FROM entries");

    // Persist dim/precision so load_index can reconstruct the vector layout.
    {
        Db::Stmt m(d, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
        m.bind(1, std::string("dim"));
        m.bind(2, static_cast<int64_t>(index.dim));
        m.step();
        m.reset();
        m.bind(1, std::string("precision"));
        m.bind(2, static_cast<int64_t>(index.precision));
        m.step();
    }

    Db::Stmt ins(d,
        "INSERT INTO entries(proto, path, chunk, line, end_line, hash, embedding) "
        "VALUES(?, ?, ?, ?, ?, ?, ?)");
    for (const auto &e : index.entries) {
        ins.reset();
        ins.bind(1, static_cast<int64_t>(e.proto));
        ins.bind(2, e.path);
        ins.bind(3, e.chunk);
        ins.bind(4, static_cast<int64_t>(e.line));
        ins.bind(5, static_cast<int64_t>(e.end_line));
        ins.bind(6, static_cast<int64_t>(e.hash));

        // Embedding stored as raw bytes: dim × sizeof(precision). Matches the
        // .emb encoding (f16 half-precision or f32) so no precision is lost.
        if (index.dim > 0 && e.embedding.size() >= index.dim) {
            if (index.precision == EmbedPrecision::F16) {
                std::vector<uint16_t> half(index.dim);
                for (uint32_t i = 0; i < index.dim; i++)
                    half[i] = float_to_f16(e.embedding[i]);
                ins.bind(7, half.data(), static_cast<int>(index.dim * sizeof(uint16_t)));
            } else {
                ins.bind(7, e.embedding.data(),
                         static_cast<int>(index.dim * sizeof(float)));
            }
        } else {
            ins.bind_null(7);
        }
        ins.step();
    }

    // entries_fts is an external-content FTS5 index over entries(path, chunk).
    // We just rewrote entries wholesale (DELETE + re-INSERT), so the FTS index
    // is stale until rebuilt. 'rebuild' re-derives the whole index from the
    // content table inside this same transaction (atomic with the entries
    // write). This is what makes FTS5 the live keyword engine for SQLite indexes.
    d.exec("INSERT INTO entries_fts(entries_fts) VALUES('rebuild')");
}

void SqliteBackend::save_index_meta_only(const std::string & /*root_path*/,
                                         const Index &index) {
    Db &d = db();
    Db::Tx tx(d);
    save_index_meta_only_locked(index);
    tx.commit();
}

void SqliteBackend::save_index_meta_only_locked(const Index &index) {
    // Rewrite entry metadata WITHOUT touching the embedding BLOBs. Mirrors
    // FileBackend's save_index_meta_only (rewrites .meta, leaves .emb alone).
    // Implemented as delete-then-insert-preserving-embeddings inside one tx:
    // read existing embeddings keyed by rowid position is fragile, so instead
    // we key preserved embeddings by (path, line) — the entry identity used
    // everywhere else — and re-attach them to the new metadata rows.
    Db &d = db();

    // Snapshot current embeddings by (path,line).
    std::map<std::pair<std::string, int64_t>, std::string> keep;
    {
        Db::Stmt q(d, "SELECT path, line, embedding FROM entries");
        while (q.step()) {
            std::string blob = column_blob(q, 2);
            if (!blob.empty())
                keep[{q.column_text(0), q.column_int64(1)}] = std::move(blob);
        }
    }

    d.exec("DELETE FROM entries");

    // Preserve dim/precision (unchanged by a meta-only save, but keep them
    // authoritative in case this is the first write).
    {
        Db::Stmt m(d, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
        m.bind(1, std::string("dim"));
        m.bind(2, static_cast<int64_t>(index.dim));
        m.step();
        m.reset();
        m.bind(1, std::string("precision"));
        m.bind(2, static_cast<int64_t>(index.precision));
        m.step();
    }

    Db::Stmt ins(d,
        "INSERT INTO entries(proto, path, chunk, line, end_line, hash, embedding) "
        "VALUES(?, ?, ?, ?, ?, ?, ?)");
    for (const auto &e : index.entries) {
        ins.reset();
        ins.bind(1, static_cast<int64_t>(e.proto));
        ins.bind(2, e.path);
        ins.bind(3, e.chunk);
        ins.bind(4, static_cast<int64_t>(e.line));
        ins.bind(5, static_cast<int64_t>(e.end_line));
        ins.bind(6, static_cast<int64_t>(e.hash));
        auto it = keep.find({e.path, static_cast<int64_t>(e.line)});
        if (it != keep.end())
            ins.bind(7, it->second.data(), static_cast<int>(it->second.size()));
        else
            ins.bind_null(7);
        ins.step();
    }

    // Same as save_index_locked: entries was rewritten, so re-derive the
    // external-content FTS index to match (keeps keyword search consistent
    // after an rm / meta-only rewrite).
    d.exec("INSERT INTO entries_fts(entries_fts) VALUES('rebuild')");
}

Index SqliteBackend::load_index() {
    Index idx;
    idx.name_ = name_;
    Db &d = db();

    // dim + precision from meta.
    {
        Db::Stmt q(d, "SELECT key, value FROM meta");
        while (q.step()) {
            std::string k = q.column_text(0);
            int64_t v = q.column_int64(1);
            if (k == "dim") idx.dim = static_cast<uint32_t>(v);
            else if (k == "precision") idx.precision = static_cast<EmbedPrecision>(v);
        }
    }

    Db::Stmt q(d,
        "SELECT proto, path, chunk, line, end_line, hash, embedding "
        "FROM entries ORDER BY rowid");
    while (q.step()) {
        IndexEntry e;
        e.proto = static_cast<EntryType>(q.column_int64(0));
        e.path = q.column_text(1);
        e.chunk = q.column_text(2);
        e.line = static_cast<uint32_t>(q.column_int64(3));
        e.end_line = static_cast<uint32_t>(q.column_int64(4));
        e.hash = static_cast<uint64_t>(q.column_int64(5));

        // Eager embedding load (documented difference from FileBackend). Decode
        // the raw bytes per the stored precision.
        std::string blob = column_blob(q, 6);
        if (idx.dim > 0 && !blob.empty()) {
            e.embedding.resize(idx.dim);
            if (idx.precision == EmbedPrecision::F16) {
                const auto *half = reinterpret_cast<const uint16_t *>(blob.data());
                size_t n = blob.size() / sizeof(uint16_t);
                for (uint32_t i = 0; i < idx.dim && i < n; i++)
                    e.embedding[i] = f16_to_float(half[i]);
            } else {
                size_t n = blob.size() / sizeof(float);
                std::memcpy(e.embedding.data(), blob.data(),
                            std::min<size_t>(idx.dim, n) * sizeof(float));
            }
        }
        idx.entries.push_back(std::move(e));
    }

    idx.emb_loaded = true; // embeddings materialized eagerly
    return idx;
}

IndexCounts SqliteBackend::load_index_counts() {
    IndexCounts c;
    Db &d = db();
    {
        Db::Stmt q(d, "SELECT value FROM meta WHERE key = 'dim'");
        if (q.step()) c.dim = static_cast<uint32_t>(q.column_int64(0));
    }
    Db::Stmt q(d, "SELECT proto, COUNT(*) FROM entries GROUP BY proto");
    while (q.step()) {
        auto proto = static_cast<EntryType>(q.column_int64(0));
        auto n = static_cast<uint32_t>(q.column_int64(1));
        if (proto == EntryType::DOC) c.file_count += n;
        else c.mem_count += n;
    }
    return c;
}

// ============================================================================
// mem / notes  (was .mem)  — PRECIOUS
// ============================================================================

void SqliteBackend::save_mem(const std::map<std::string, MemEntry> &store) {
    Db &d = db();
    Db::Tx tx(d);
    save_mem_locked(store);
    tx.commit();
}

void SqliteBackend::save_mem_locked(const std::map<std::string, MemEntry> &store) {
    Db &d = db();
    d.exec("DELETE FROM notes");
    Db::Stmt ins(d,
        "INSERT INTO notes(topic, content, timestamp, sources) VALUES(?, ?, ?, ?)");
    for (const auto &[topic, entry] : store) {
        ins.reset();
        ins.bind(1, topic);
        ins.bind(2, entry.content);
        ins.bind(3, entry.timestamp);
        ins.bind(4, join_sep(entry.sources)); // sources list → unit-sep string
        ins.step();
    }
}

bool SqliteBackend::load_mem(std::map<std::string, MemEntry> &store) {
    store.clear();
    Db &d = db();
    Db::Stmt q(d, "SELECT topic, content, timestamp, sources FROM notes");
    while (q.step()) {
        MemEntry e;
        std::string topic = q.column_text(0);
        e.content = q.column_text(1);
        e.timestamp = q.column_int64(2);
        e.sources = split_sep(q.column_text(3));
        store[topic] = std::move(e);
    }
    return true;
}

// ============================================================================
// glossary  (was .glossary)  — PRECIOUS
// ============================================================================

void SqliteBackend::save_glossary(const std::map<std::string, std::string> &glossary) {
    Db &d = db();
    Db::Tx tx(d);
    save_glossary_locked(glossary);
    tx.commit();
}

void SqliteBackend::save_glossary_locked(const std::map<std::string, std::string> &glossary) {
    Db &d = db();
    d.exec("DELETE FROM glossary");
    Db::Stmt ins(d, "INSERT INTO glossary(term, expansion) VALUES(?, ?)");
    for (const auto &[term, expansion] : glossary) {
        ins.reset();
        ins.bind(1, term);
        ins.bind(2, expansion);
        ins.step();
    }
}

std::map<std::string, std::string> SqliteBackend::load_glossary() {
    std::map<std::string, std::string> out;
    Db &d = db();
    Db::Stmt q(d, "SELECT term, expansion FROM glossary");
    while (q.step())
        out[q.column_text(0)] = q.column_text(1);
    return out;
}

// ============================================================================
// edges: call / import / docref / type  (was .calls / .imports / .docrefs / .types)
// ============================================================================

namespace {
// Shared insert/delete SQL for the unified edges table (no duplicated strings).
constexpr const char *kInsertEdge =
    "INSERT INTO edges(kind, src, dst, file, line, attrs) VALUES(?, ?, ?, ?, ?, ?)";
constexpr const char *kSelectEdge =
    "SELECT src, dst, file, line, attrs FROM edges WHERE kind = ?";

void delete_kind(Db &d, const char *kind) {
    Db::Stmt del(d, "DELETE FROM edges WHERE kind = ?");
    del.bind(1, std::string(kind));
    del.step();
}
} // namespace

// --- call graph -------------------------------------------------------------

void SqliteBackend::save_call_graph(const CallGraph &graph) {
    Db &d = db();
    Db::Tx tx(d);
    save_call_graph_locked(graph);
    tx.commit();
}

void SqliteBackend::save_call_graph_locked(const CallGraph &graph) {
    Db &d = db();
    delete_kind(d, kKindCall);
    Db::Stmt ins(d, kInsertEdge);
    for (const auto &e : graph.edges) {
        ins.reset();
        ins.bind(1, std::string(kKindCall));
        ins.bind(2, e.caller); // src
        ins.bind(3, e.callee); // dst
        ins.bind(4, e.file);
        ins.bind(5, static_cast<int64_t>(e.line));
        ins.bind_null(6); // no extra attrs
        ins.step();
    }
}

CallGraph SqliteBackend::load_call_graph() {
    CallGraph g;
    Db &d = db();
    Db::Stmt q(d, kSelectEdge);
    q.bind(1, std::string(kKindCall));
    while (q.step()) {
        StoredCallEdge e;
        e.caller = q.column_text(0);
        e.callee = q.column_text(1);
        e.file = q.column_text(2);
        e.line = static_cast<uint32_t>(q.column_int64(3));
        g.edges.push_back(std::move(e));
    }
    return g;
}

// --- import graph -----------------------------------------------------------
// One row per ImportInfo. src=file, dst=module_path, attrs=symbols (unit-sep).

void SqliteBackend::save_import_graph(const ImportGraph &graph) {
    Db &d = db();
    Db::Tx tx(d);
    save_import_graph_locked(graph);
    tx.commit();
}

void SqliteBackend::save_import_graph_locked(const ImportGraph &graph) {
    Db &d = db();
    delete_kind(d, kKindImport);
    Db::Stmt ins(d, kInsertEdge);
    for (const auto &[file, imports] : graph.imports) {
        for (const auto &imp : imports) {
            ins.reset();
            ins.bind(1, std::string(kKindImport));
            ins.bind(2, file);            // src
            ins.bind(3, imp.module_path); // dst
            ins.bind_null(4);             // file column unused (src holds it)
            ins.bind(5, static_cast<int64_t>(0));
            ins.bind(6, join_sep(imp.symbols)); // named symbols
            ins.step();
        }
    }
}

ImportGraph SqliteBackend::load_import_graph() {
    ImportGraph g;
    Db &d = db();
    Db::Stmt q(d, kSelectEdge);
    q.bind(1, std::string(kKindImport));
    while (q.step()) {
        std::string file = q.column_text(0);
        ImportInfo imp;
        imp.module_path = q.column_text(1);
        imp.symbols = split_sep(q.column_text(4)); // attrs
        g.imports[file].push_back(std::move(imp));
    }
    return g;
}

// --- docref graph -----------------------------------------------------------
// src=source_file, dst=target, attrs="<refkind>\x1f<text>".

void SqliteBackend::save_docref_graph(const DocRefGraph &graph) {
    Db &d = db();
    Db::Tx tx(d);
    save_docref_graph_locked(graph);
    tx.commit();
}

void SqliteBackend::save_docref_graph_locked(const DocRefGraph &graph) {
    Db &d = db();
    delete_kind(d, kKindDocref);
    Db::Stmt ins(d, kInsertEdge);
    for (const auto &[file, refs] : graph.refs) {
        for (const auto &r : refs) {
            ins.reset();
            ins.bind(1, std::string(kKindDocref));
            ins.bind(2, file);     // src
            ins.bind(3, r.target); // dst
            ins.bind_null(4);
            ins.bind(5, static_cast<int64_t>(0));
            // attrs = refkind (0/1) + separator + link text
            std::vector<std::string> attr = {
                std::to_string(static_cast<int>(r.kind)), r.text};
            ins.bind(6, join_sep(attr));
            ins.step();
        }
    }
}

DocRefGraph SqliteBackend::load_docref_graph() {
    DocRefGraph g;
    Db &d = db();
    Db::Stmt q(d, kSelectEdge);
    q.bind(1, std::string(kKindDocref));
    while (q.step()) {
        std::string file = q.column_text(0);
        DocRefEdge e;
        e.target = q.column_text(1);
        auto parts = split_sep(q.column_text(4)); // attrs
        e.kind = (!parts.empty() && parts[0] == "1") ? RefKind::External : RefKind::Local;
        e.text = parts.size() > 1 ? parts[1] : std::string();
        g.refs[file].push_back(std::move(e));
    }
    return g;
}

// --- type graph -------------------------------------------------------------
// src=name, dst=parent, attrs=kind ("extends"/"implements").

void SqliteBackend::save_type_graph(const TypeGraph &graph) {
    Db &d = db();
    Db::Tx tx(d);
    save_type_graph_locked(graph);
    tx.commit();
}

void SqliteBackend::save_type_graph_locked(const TypeGraph &graph) {
    Db &d = db();
    delete_kind(d, kKindType);
    Db::Stmt ins(d, kInsertEdge);
    for (const auto &e : graph.edges) {
        ins.reset();
        ins.bind(1, std::string(kKindType));
        ins.bind(2, e.name);   // src
        ins.bind(3, e.parent); // dst
        ins.bind(4, e.file);
        ins.bind(5, static_cast<int64_t>(e.line));
        ins.bind(6, e.kind); // "extends" / "implements"
        ins.step();
    }
}

TypeGraph SqliteBackend::load_type_graph() {
    TypeGraph g;
    Db &d = db();
    Db::Stmt q(d, kSelectEdge);
    q.bind(1, std::string(kKindType));
    while (q.step()) {
        StoredTypeEdge e;
        e.name = q.column_text(0);
        e.parent = q.column_text(1);
        e.file = q.column_text(2);
        e.line = static_cast<uint32_t>(q.column_int64(3));
        e.kind = q.column_text(4); // attrs
        g.edges.push_back(std::move(e));
    }
    return g;
}

// ============================================================================
// exports  (was .exports)
// ============================================================================

void SqliteBackend::save_export_store(const ExportStore &store) {
    Db &d = db();
    Db::Tx tx(d);
    save_export_store_locked(store);
    tx.commit();
}

void SqliteBackend::save_export_store_locked(const ExportStore &store) {
    Db &d = db();
    d.exec("DELETE FROM exports");
    Db::Stmt ins(d, "INSERT INTO exports(file, symbol) VALUES(?, ?)");
    for (const auto &[file, symbols] : store.exports) {
        for (const auto &sym : symbols) {
            ins.reset();
            ins.bind(1, file);
            ins.bind(2, sym);
            ins.step();
        }
    }
}

ExportStore SqliteBackend::load_export_store() {
    ExportStore store;
    Db &d = db();
    Db::Stmt q(d, "SELECT file, symbol FROM exports");
    while (q.step())
        store.exports[q.column_text(0)].push_back(q.column_text(1));
    return store;
}

// ============================================================================
// metrics  (was .metrics)
// ============================================================================

void SqliteBackend::save_metrics(const MetricsStore &store) {
    Db &d = db();
    Db::Tx tx(d);
    save_metrics_locked(store);
    tx.commit();
}

void SqliteBackend::save_metrics_locked(const MetricsStore &store) {
    Db &d = db();
    d.exec("DELETE FROM metrics");
    Db::Stmt ins(d,
        "INSERT INTO metrics(file, name, complexity, lines, params, returns, max_depth, line) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
    for (const auto &m : store.entries) {
        ins.reset();
        ins.bind(1, m.file);
        ins.bind(2, m.name);
        ins.bind(3, static_cast<int64_t>(m.complexity));
        ins.bind(4, static_cast<int64_t>(m.lines));
        ins.bind(5, static_cast<int64_t>(m.params));
        ins.bind(6, static_cast<int64_t>(m.returns));
        ins.bind(7, static_cast<int64_t>(m.max_depth));
        ins.bind(8, static_cast<int64_t>(m.line));
        ins.step();
    }
}

MetricsStore SqliteBackend::load_metrics() {
    MetricsStore store;
    Db &d = db();
    Db::Stmt q(d,
        "SELECT file, name, complexity, lines, params, returns, max_depth, line FROM metrics");
    while (q.step()) {
        FunctionMetrics m;
        m.file = q.column_text(0);
        m.name = q.column_text(1);
        m.complexity = static_cast<uint16_t>(q.column_int64(2));
        m.lines = static_cast<uint16_t>(q.column_int64(3));
        m.params = static_cast<uint8_t>(q.column_int64(4));
        m.returns = static_cast<uint8_t>(q.column_int64(5));
        m.max_depth = static_cast<uint8_t>(q.column_int64(6));
        m.line = static_cast<uint32_t>(q.column_int64(7));
        store.entries.push_back(std::move(m));
    }
    return store;
}

// ============================================================================
// Whole-index load/save + schema version
// ============================================================================

IndexData SqliteBackend::load_all() {
    // Reuse the per-collection loaders (no duplicated parsing). load_index()
    // already materializes embeddings eagerly, so IndexData is fully populated.
    IndexData data;
    data.index = load_index();
    load_mem(data.mem);
    data.glossary = load_glossary();
    data.calls = load_call_graph();
    data.imports = load_import_graph();
    data.docrefs = load_docref_graph();
    data.exports = load_export_store();
    data.types = load_type_graph();
    data.metrics = load_metrics();
    return data;
}

void SqliteBackend::save_all(const std::string & /*root_path*/, const IndexData &data) {
    // ATOMICITY GUARANTEE: every collection is written inside ONE transaction,
    // so the whole index commits all-or-nothing. A crash mid-save rolls back
    // cleanly — the data-layer guarantee FileBackend cannot make (it only has
    // per-file atomic_write, no cross-file transaction). The *_locked helpers
    // hold the SQL and open NO transaction of their own (SQLite has no nestable
    // BEGIN), so they compose safely under this single outer Tx.
    Db &d = db();
    Db::Tx tx(d);
    save_index_locked(data.index);
    save_mem_locked(data.mem);
    save_glossary_locked(data.glossary);
    save_call_graph_locked(data.calls);
    save_import_graph_locked(data.imports);
    save_docref_graph_locked(data.docrefs);
    save_export_store_locked(data.exports);
    save_type_graph_locked(data.types);
    save_metrics_locked(data.metrics);
    tx.commit();
}

// Schema version maps natively to PRAGMA user_version (a never-written DB reads
// back 0). No sidecar file needed — the version lives in the .db itself.
uint32_t SqliteBackend::schema_version() { return db().user_version(); }
void SqliteBackend::set_schema_version(uint32_t version) {
    db().set_user_version(version);
}

// ============================================================================
// FTS5 keyword search  (the live keyword engine for SQLite-backed indexes)
// ============================================================================

namespace {
// Build a safe FTS5 MATCH expression from a free-text query. Each whitespace-
// separated term is wrapped in double quotes so FTS5 treats it as a bare string
// literal — this neutralizes FTS5 operators/special chars (AND, OR, NOT, NEAR,
// *, ^, :, parentheses, hyphens) that would otherwise be a syntax error or
// change semantics. Embedded double quotes are escaped by doubling ("" per FTS5
// string-literal rules). Terms are OR-joined (any term may match), i.e.
// `" OR ".join('"%s"' % t for t in query.split())` in effect. Short (<=1 char)
// terms and pure-quote terms are dropped. Returns "" when nothing usable
// remains (caller then returns no results rather than issuing a bad MATCH).
std::string build_fts_match(const std::string &query) {
    std::string out;
    size_t i = 0;
    const size_t n = query.size();
    while (i < n) {
        // skip whitespace
        while (i < n && std::isspace(static_cast<unsigned char>(query[i]))) i++;
        size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(query[i]))) i++;
        if (i <= start) continue;
        std::string term = query.substr(start, i - start);
        if (term.size() < 2) continue; // drop 1-char noise

        // Escape embedded double quotes by doubling them, then wrap in quotes.
        std::string quoted = "\"";
        for (char c : term) {
            if (c == '"') quoted += "\"\"";
            else quoted += c;
        }
        quoted += "\"";

        if (!out.empty()) out += " OR ";
        out += quoted;
    }
    return out;
}
} // namespace

std::vector<std::pair<int, double>>
SqliteBackend::keyword_search_fts(const std::string &query, int limit) {
    std::vector<std::pair<int, double>> results;
    if (limit <= 0) return results;

    const std::string match = build_fts_match(query);
    if (match.empty()) return results;

    Db &d = db();
    // rank is FTS5's bm25 score: MORE NEGATIVE = better. ORDER BY rank ASC gives
    // best-first. We map entries.rowid (== FTS content_rowid) back to the dense
    // 0-based entry index used by the searcher (rowid is 1..N in insert order,
    // and load_index reads ORDER BY rowid, so entry_index = rowid - 1). We
    // return -rank so larger = better (mirrors keyword_search()'s BM25 sign);
    // only rank ORDER is consumed by RRF, so the exact magnitude is immaterial.
    Db::Stmt q(d,
        "SELECT rowid, rank FROM entries_fts "
        "WHERE entries_fts MATCH ? ORDER BY rank LIMIT ?");
    q.bind(1, match);                     // bound param — never concatenated
    q.bind(2, static_cast<int64_t>(limit));
    while (q.step()) {
        int64_t rowid = q.column_int64(0);
        double rank = sqlite3_column_double(q.raw(), 1);
        int entry_index = static_cast<int>(rowid - 1); // rowid is 1-based dense
        if (entry_index >= 0)
            results.emplace_back(entry_index, -rank);
    }
    return results;
}
