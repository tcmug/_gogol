// sqlite_backend.h — StorageBackend that persists ONE index to a single
// <index>.db SQLite database (via the Db RAII wrapper).
//
// This is the Phase 3 implementation (see docs/design/storage-architecture.md).
// It mirrors FileBackend's observable behavior exactly — the same in-memory
// structs go in and come back out — but stores everything in one file instead
// of the 11-file pile FileBackend writes.
//
// DB path: ~/.gogol/indexes/<index>.db   (mirrors mem_path/imports_path).
//
// Schema is created by Db::ensure_schema() (see db.cpp). This backend adds one
// small extra table, `meta(key,value)`, to persist Index.dim + Index.precision
// (the .emb header fields that have no natural home in the row-per-entry
// `entries` table).
//
// --- Behavior notes / documented differences from FileBackend ---
//
// * Embeddings are loaded EAGERLY in load_index() (FileBackend loads them lazily
//   via Index::ensure_embeddings(), which reads the sidecar .emb through
//   index_file). There is no .emb sidecar here, so load_index() reads the
//   embedding BLOB column directly, fills IndexEntry::embedding, and sets
//   emb_loaded=true. This is behavior-preserving at the query level: the daemon
//   calls ensure_embeddings() before cosine anyway, so the embeddings it sees
//   are identical; they are merely materialized earlier.
//
// * Every save_* wraps its writes in a single Db::Tx (atomic). A crash mid-save
//   rolls back cleanly — the atomicity win the design doc calls out.
#pragma once
#include "storage/db.h"
#include "storage/storage_backend.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class SqliteBackend : public StorageBackend {
public:
    explicit SqliteBackend(std::string index);

    const std::string &index_name() const override { return name_; }

    // --- Loaders ---
    Index load_index() override;
    IndexCounts load_index_counts() override;
    bool load_mem(std::map<std::string, MemEntry> &store) override;
    std::map<std::string, std::string> load_glossary() override;
    CallGraph load_call_graph() override;
    ImportGraph load_import_graph() override;
    DocRefGraph load_docref_graph() override;
    ExportStore load_export_store() override;
    TypeGraph load_type_graph() override;
    MetricsStore load_metrics() override;

    // --- Savers ---
    void save_index(const std::string &root_path, const Index &index) override;
    void save_index_meta_only(const std::string &root_path, const Index &index) override;
    void save_mem(const std::map<std::string, MemEntry> &store) override;
    void save_glossary(const std::map<std::string, std::string> &glossary) override;
    void save_call_graph(const CallGraph &graph) override;
    void save_import_graph(const ImportGraph &graph) override;
    void save_docref_graph(const DocRefGraph &graph) override;
    void save_export_store(const ExportStore &store) override;
    void save_type_graph(const TypeGraph &graph) override;
    void save_metrics(const MetricsStore &store) override;

    // --- Whole-index load/save ---
    //
    // save_all wraps EVERY collection's write in ONE Db::Tx, so the whole index
    // commits atomically (all-or-nothing) — the key data-layer guarantee this
    // backend provides over FileBackend. load_all reuses the per-collection
    // loaders.
    IndexData load_all() override;
    void save_all(const std::string &root_path, const IndexData &data) override;

    // --- Schema version (delegates to PRAGMA user_version) ---
    uint32_t schema_version() override;
    void set_schema_version(uint32_t version) override;

    // FTS5 keyword search over entries_fts (path + chunk). Returns
    // (entry_index, score) pairs sorted best-first, where entry_index is the
    // 0-based position in load_index()'s entry vector (i.e. rowid - 1, since
    // entries.rowid is a dense 1..N sequence assigned by save_index_locked and
    // load_index reads ORDER BY rowid). This is the ONLY keyword ranking source
    // on the search path (the in-memory BM25 index was removed). `score` is the
    // (negated) FTS5 bm25 rank so that larger = better; only the RANK ORDER is
    // used by RRF, so the exact magnitude is immaterial.
    //
    // The MATCH query is built defensively from the user query: each term is
    // double-quoted (so FTS5 treats it as a literal, never as an operator) and
    // the terms are OR-joined —
    // `" OR ".join('"%s"' % t ...)` in effect. The assembled string is passed as a BOUND
    // parameter (never concatenated into the SQL text). An empty/again-empty
    // query yields no results.
    std::vector<std::pair<int, double>>
    keyword_search_fts(const std::string &query, int limit);

    // Absolute path of the .db this backend is bound to (also used by tests).
    static std::string db_path(const std::string &index);

private:
    Db &db();                     // lazily open the connection
    void ensure_meta_table();     // create meta(key,value) if absent

    // --- Per-collection writers WITHOUT their own transaction ---
    //
    // Each public save_* opens a Db::Tx then calls its *_locked helper; save_all
    // opens ONE Tx and calls every helper. SQLite has no nestable BEGIN, so the
    // SQL lives only in these helpers (no duplication) and no helper opens a Tx
    // of its own. "_locked" = "assumes the caller already holds a transaction".
    void save_index_locked(const Index &index);
    void save_index_meta_only_locked(const Index &index);
    void save_mem_locked(const std::map<std::string, MemEntry> &store);
    void save_glossary_locked(const std::map<std::string, std::string> &glossary);
    void save_call_graph_locked(const CallGraph &graph);
    void save_import_graph_locked(const ImportGraph &graph);
    void save_docref_graph_locked(const DocRefGraph &graph);
    void save_export_store_locked(const ExportStore &store);
    void save_type_graph_locked(const TypeGraph &graph);
    void save_metrics_locked(const MetricsStore &store);

    std::string name_;
    std::unique_ptr<Db> db_;
};
