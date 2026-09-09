// indexer.h — Shared indexing pipeline (scan → detect changes → embed → save)
#pragma once
#include "config/config.h"
#include "embedding/embed_provider.h"
#include "storage/index_file.h"
#include <functional>
#include <string>

struct IndexResult {
    int embedded = 0;
    int skipped = 0;
    int total = 0;
};

// Progress callback: (current, total)
using IndexProgressFn = std::function<void(int, int)>;

// Run full index pipeline for a single named index.
// Mutates index in place. Persists to disk. FTS5 keyword search is rebuilt
// inside the SqliteBackend save path, so run_index no longer produces a
// separate in-memory keyword index.
// progress_name: shown in progress output (empty = silent).
IndexResult run_index(const std::string &name, const IndexConfig &cfg,
                      EmbedProvider &embedder, Index &index, bool force,
                      const std::string &progress_name = "",
                      IndexProgressFn progress_fn = nullptr);
