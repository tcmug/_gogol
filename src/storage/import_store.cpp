// Read-only: loaders retained for migration; writes go through SqliteBackend.
// import_store.cpp — Per-index import graph storage (TSV format)
#include "storage/import_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::vector<std::string> ImportGraph::imports_of(const std::string &file) const {
  std::vector<std::string> result;
  auto it = imports.find(file);
  if (it == imports.end()) return result;
  for (auto &imp : it->second)
    result.push_back(imp.module_path);
  return result;
}

std::vector<std::string> ImportGraph::imported_by(const std::string &path) const {
  // Import module paths are stored RELATIVE to the importing file (e.g.
  // "../../data-sources/store-data-source", usually without extension), so we
  // cannot compare them against a full index-relative path directly. We match
  // on the trailing path segments (extension-stripped): the module path must
  // end with the queried file's basename, AND — when the query carries more
  // than a bare filename — the preceding directory segment must agree too, so
  // "a/foo" does not match an unrelated "b/foo". This keeps relative-import
  // resolution working while avoiding the bare-filename over-match that made
  // `affected` report unrelated same-named files.
  auto strip_ext = [](const std::string &s) {
    auto slash = s.rfind('/');
    auto dot = s.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      return s.substr(0, dot);
    return s;
  };
  auto last_seg = [](const std::string &s) {
    auto slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
  };
  auto parent_seg = [](const std::string &s) -> std::string {
    auto slash = s.rfind('/');
    if (slash == std::string::npos) return "";
    auto slash2 = s.rfind('/', slash - 1);
    return s.substr(slash2 == std::string::npos ? 0 : slash2 + 1, slash - (slash2 == std::string::npos ? 0 : slash2 + 1));
  };

  const std::string q = strip_ext(path);
  const std::string q_base = last_seg(q);
  const std::string q_parent = parent_seg(q); // "" if query is a bare filename

  std::vector<std::string> result;
  for (auto &[file, file_imports] : imports) {
    for (auto &imp : file_imports) {
      const std::string m = strip_ext(imp.module_path);
      if (last_seg(m) != q_base) continue;        // basenames must match
      if (!q_parent.empty() && parent_seg(m) != q_parent) continue; // and dir if given
      result.push_back(file);
      break;
    }
  }
  return result;
}

std::vector<std::string> ImportGraph::files_importing_symbol(const std::string &symbol) const {
  std::vector<std::string> result;
  for (auto &[file, file_imports] : imports) {
    for (auto &imp : file_imports) {
      for (auto &sym : imp.symbols) {
        if (sym == symbol) { result.push_back(file); break; }
      }
    }
  }
  return result;
}
