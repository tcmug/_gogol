// Read-only: loaders retained for migration; writes go through SqliteBackend.
// metrics_store.cpp — Per-index function metrics storage (TSV format)
#include "storage/metrics_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::vector<FunctionMetrics> MetricsStore::for_file(const std::string &file) const {
  std::vector<FunctionMetrics> result;
  for (auto &m : entries)
    if (m.file == file || m.file.find(file) != std::string::npos)
      result.push_back(m);
  return result;
}

FunctionMetrics *MetricsStore::find(const std::string &name) {
  for (auto &m : entries)
    if (m.name == name) return &m;
  return nullptr;
}
