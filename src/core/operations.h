// operations.h — Shared add/rm/get logic used by both CLI and daemon
#pragma once
#include "config/config.h"
#include "core/loc.h"
#include "embedding/embed_provider.h"
#include "storage/index_file.h"
#include <string>
#include <vector>

struct OpResult {
  bool ok = false;
  std::string message;
};

// Add an entry (doc file, note, or term). Updates index in-place.
OpResult op_add(EntryType type, const std::string &index,
                const std::string &path, const std::string &content,
                const std::string &sources, const IndexConfig &config,
                EmbedProvider &embedder, Index &index_data);

// Remove an entry (doc file, note, or term). Updates index in-place.
OpResult op_rm(EntryType type, const std::string &index,
               const std::string &path, const IndexConfig &config,
               Index &index_data);

// Get entry content (doc file, note, or term).
OpResult op_get(EntryType type, const std::string &index,
                const std::string &path, const IndexConfig &config,
                int max_lines = 0);
