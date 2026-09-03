// index_data.h — Aggregate of everything an index holds in memory.
//
// This bundles the existing per-store types (no new serialization) so a
// StorageBackend can expose a single load/save payload if it wants to. It
// reuses the existing structs verbatim — it is purely an aggregation.
#pragma once
#include "storage/call_store.h"
#include "storage/docref_store.h"
#include "storage/export_store.h"
#include "storage/glossary_store.h" // (map<string,string> lives here conceptually)
#include "storage/import_store.h"
#include "storage/index_file.h"
#include "storage/mem_store.h"
#include "storage/metrics_store.h"
#include "storage/type_store.h"

#include <map>
#include <string>

// Everything an index holds in memory, aggregated. This is the load_all/save_all
// payload. Members reuse the existing store types unchanged.
struct IndexData {
    Index index;                              // entries + embeddings (.meta/.emb)
    std::map<std::string, MemEntry> mem;      // memory notes (.mem)
    std::map<std::string, std::string> glossary; // glossary terms (.glossary)
    CallGraph calls;                          // call edges (.calls)
    ImportGraph imports;                      // import edges (.imports)
    DocRefGraph docrefs;                      // doc reference edges (.docrefs)
    ExportStore exports;                      // exported symbols (.exports)
    TypeGraph types;                          // type hierarchy edges (.types)
    MetricsStore metrics;                     // function metrics (.metrics)
};
