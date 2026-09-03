// storage_backend.h — Swappable per-index storage backend.
//
// The daemon, indexer and CLI depend ONLY on this abstract interface, never on
// the concrete per-file store functions (load_call_graph, save_mem_store, ...).
// One StorageBackend instance == one index. Backends are obtained from the
// open_backend() factory, which selects the implementation from config.
//
// Phase 1 shipped FileBackend (a per-file pass-through); it has since been
// removed. SqliteBackend is now the sole implementation — one <index>.db
// SQLite database per index (see docs/design/storage-architecture.md).
//
// The methods mirror the current store free-function signatures 1:1 so the
// refactor is mechanical and behavior-preserving.
#pragma once
#include "storage/call_store.h"
#include "storage/docref_store.h"
#include "storage/export_store.h"
#include "storage/import_store.h"
#include "storage/index_data.h"
#include "storage/index_file.h"
#include "storage/mem_store.h"
#include "storage/metrics_store.h"
#include "storage/type_store.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

// Current per-index data-layer schema version. Written to a backend's
// schema_version() (PRAGMA user_version for SqliteBackend). Bump this when
// gogol's extraction logic changes in a way that requires re-extraction.
// A .db whose schema_version() equals this constant is considered "current"
// and is used directly (no migration / re-extract). A never-written backend
// reads back 0, which is < INDEXER_VERSION, so it is treated as needing work.
constexpr uint32_t INDEXER_VERSION = 1;

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    // The index this backend is bound to.
    virtual const std::string &index_name() const = 0;

    // --- Loaders (mirror the current free functions 1:1) ---
    virtual Index load_index() = 0;
    virtual IndexCounts load_index_counts() = 0;
    virtual bool load_mem(std::map<std::string, MemEntry> &store) = 0;
    virtual std::map<std::string, std::string> load_glossary() = 0;
    virtual CallGraph load_call_graph() = 0;
    virtual ImportGraph load_import_graph() = 0;
    virtual DocRefGraph load_docref_graph() = 0;
    virtual ExportStore load_export_store() = 0;
    virtual TypeGraph load_type_graph() = 0;
    virtual MetricsStore load_metrics() = 0;

    // --- Savers (mirror the current free functions 1:1) ---
    virtual void save_index(const std::string &root_path, const Index &index) = 0;
    virtual void save_index_meta_only(const std::string &root_path, const Index &index) = 0;
    virtual void save_mem(const std::map<std::string, MemEntry> &store) = 0;
    virtual void save_glossary(const std::map<std::string, std::string> &glossary) = 0;
    virtual void save_call_graph(const CallGraph &graph) = 0;
    virtual void save_import_graph(const ImportGraph &graph) = 0;
    virtual void save_docref_graph(const DocRefGraph &graph) = 0;
    virtual void save_export_store(const ExportStore &store) = 0;
    virtual void save_type_graph(const TypeGraph &graph) = 0;
    virtual void save_metrics(const MetricsStore &store) = 0;

    // --- Whole-index load/save ---
    //
    // save_all persists every collection ATOMICALLY (all-or-nothing) as far as
    // the backend can guarantee. load_all returns a fully-materialized
    // IndexData (embeddings included). These are convenience aggregations built
    // on the per-collection load/save methods above; the atomicity guarantee is
    // backend-specific:
    //   * SqliteBackend — a single transaction wraps every collection, so the
    //     whole index commits atomically (a crash mid-save rolls back cleanly).
    //   * FileBackend   — per-file atomic_write only; there is NO cross-file
    //     transaction (an honest limitation of the pile-of-files layout).
    virtual IndexData load_all() = 0;
    virtual void save_all(const std::string &root_path, const IndexData &data) = 0;

    // --- Per-index data-layer schema version ---
    //
    // Drives auto re-extract when gogol's extraction logic changes. The backend
    // chooses the storage medium (a sidecar file, PRAGMA user_version, ...).
    // A never-written version reads back as 0.
    virtual uint32_t schema_version() = 0;
    virtual void set_schema_version(uint32_t version) = 0;
};

// Factory: returns the backend for an index. Selected from config.
//
// TODO(sqlite): when the SqliteBackend lands, this factory reads the per-index
// backend selector from config, e.g.
//
//     [<index>]
// SQLite-only: open_backend always returns a SqliteBackend (one <index>.db per
// index). The legacy FileBackend and the backend-selection config/env override
// were removed (see storage-architecture.md).
std::unique_ptr<StorageBackend> open_backend(const std::string &index);

// Returns true when open_backend(index) would return a SqliteBackend — i.e. a
// current <index>.db exists (or GOGOL_BACKEND=sqlite forces it). Lets callers
// pick the SQLite-only code path (e.g. FTS5 keyword search) without downcasting
// the returned StorageBackend. Mirrors open_backend()'s selection logic exactly.
bool is_sqlite_backed(const std::string &index);
