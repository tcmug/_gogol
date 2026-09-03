// searcher.h — Shared search pipeline (cosine + FTS5 keyword + RRF)
#pragma once
#include "config/config.h"
#include "embedding/embed_provider.h"
#include "storage/index_file.h"
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct SearchOptions {
  std::string query;
  std::vector<std::string> index_names;
  int type_filter = -1; // -1 = all; else (int)EntryType (DOC=file, NOTE=mem)
  int top_k = 5;
  int show_lines = 0;
};

struct SearchResult {
  std::string index;    // index name
  std::string path;     // relative path (or topic for mem)
  uint32_t line = 0;    // start line (0 for mem entries)
  std::string chunk;    // chunk heading
  uint8_t proto = 0;    // 0=doc, 1=note, 2=term
  std::string status;   // "ok", "stale", "missing"
  float score = 0;
  float cosine = 0;
  std::string snippet;
};

// FTS keyword provider: given (query, limit), returns (entry_index, score)
// pairs best-first from the index's SQLite FTS5 store. entry_index is the
// 0-based position in that index's entry vector. This is the sole source of the
// keyword RRF contribution. When the map has no entry for an index, that index
// contributes cosine ranking only (no keyword term).
using FtsKeywordFn =
    std::function<std::vector<std::pair<int, double>>(const std::string &query,
                                                      int limit)>;

// Run full search pipeline. Mutates nothing.
// Caller must ensure each index's embeddings are loaded (ensure_embeddings)
// before calling — indexes are treated as immutable (borrowed const pointers).
//
// fts_providers (optional): per-index FTS5 keyword-search callbacks. For any
// index present in this map, its callback supplies the keyword RRF contribution
// (FTS5 MATCH on the SQLite DB). Indexes absent from the map contribute cosine
// ranking only. The fusion math (RRF, k=60) is unchanged.
std::vector<SearchResult> search(
    const SearchOptions &opts,
    EmbedProvider &embedder,
    const std::map<std::string, const Index *> &indexes,
    const std::map<std::string, IndexConfig> &configs,
    const std::map<std::string, FtsKeywordFn> &fts_providers = {});
