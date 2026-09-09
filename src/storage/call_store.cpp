// Read-only: loaders retained for migration; writes go through SqliteBackend.
// call_store.cpp — Per-index call-graph edge storage (TSV format)
#include "storage/call_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::vector<StoredCallEdge> CallGraph::callers_of(const std::string &name) const {
  std::vector<StoredCallEdge> result;
  for (auto &e : edges) {
    if (e.callee == name)
      result.push_back(e);
  }
  return result;
}

std::vector<StoredCallEdge> CallGraph::callees_of(const std::string &name) const {
  std::vector<StoredCallEdge> result;
  for (auto &e : edges) {
    if (e.caller == name)
      result.push_back(e);
  }
  return result;
}

// File-scoped versions: only edges from a specific file
std::vector<StoredCallEdge> CallGraph::callers_of(const std::string &name, const std::string &file) const {
  std::vector<StoredCallEdge> result;
  for (auto &e : edges) {
    if (e.callee == name && e.file == file)
      result.push_back(e);
  }
  return result;
}

std::vector<StoredCallEdge> CallGraph::callees_of(const std::string &name, const std::string &file) const {
  std::vector<StoredCallEdge> result;
  for (auto &e : edges) {
    if (e.caller == name && e.file == file)
      result.push_back(e);
  }
  return result;
}

// Find which files define a function (have it as a caller)
std::vector<std::string> CallGraph::files_defining(const std::string &name) const {
  std::set<std::string> files;
  for (auto &e : edges) {
    if (e.caller == name)
      files.insert(e.file);
  }
  return {files.begin(), files.end()};
}
