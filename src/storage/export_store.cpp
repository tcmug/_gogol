// Read-only: loaders retained for migration; writes go through SqliteBackend.
// export_store.cpp — Per-index export symbol storage (TSV format)
#include "storage/export_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::vector<std::string> ExportStore::exports_of(const std::string &file) const {
  auto it = exports.find(file);
  if (it != exports.end()) return it->second;
  return {};
}
