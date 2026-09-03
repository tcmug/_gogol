// Read-only: loaders retained for migration; writes go through SqliteBackend.
// type_store.cpp — Per-index type hierarchy storage (TSV format)
#include "storage/type_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::vector<StoredTypeEdge> TypeGraph::parents_of(const std::string &name) const {
  std::vector<StoredTypeEdge> result;
  for (auto &e : edges)
    if (e.name == name) result.push_back(e);
  return result;
}

std::vector<StoredTypeEdge> TypeGraph::children_of(const std::string &name) const {
  std::vector<StoredTypeEdge> result;
  for (auto &e : edges)
    if (e.parent == name) result.push_back(e);
  return result;
}
