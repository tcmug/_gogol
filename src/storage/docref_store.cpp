// Read-only: loaders retained for migration; writes go through SqliteBackend.
// docref_store.cpp — Per-index document reference graph storage (TSV format)
#include "storage/docref_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::vector<DocRefEdge> DocRefGraph::references_of(const std::string &file) const {
  std::vector<DocRefEdge> result;
  auto it = refs.find(file);
  if (it == refs.end()) return result;
  for (auto &edge : it->second)
    result.push_back(edge);
  return result;
}

std::vector<std::string> DocRefGraph::referenced_by(const std::string &file) const {
  std::vector<std::string> result;
  for (auto &[source, edges] : refs) {
    for (auto &edge : edges) {
      if (edge.kind == RefKind::Local && edge.target == file) {
        result.push_back(source);
        break;
      }
    }
  }
  return result;
}

// --- Doc reference resolution ---

// Normalize a slash-separated path by processing '.' and '..' segments.
// Pure string math — never touches the filesystem. A leading '..' that would
// escape the root is dropped (there is no parent above the project root).
static std::string normalize_path(const std::string &path) {
  std::vector<std::string> stack;
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    std::string seg =
        (slash == std::string::npos) ? path.substr(start) : path.substr(start, slash - start);
    if (seg.empty() || seg == ".") {
      // skip empty segments ("a//b") and current-dir markers
    } else if (seg == "..") {
      if (!stack.empty()) stack.pop_back();
    } else {
      stack.push_back(seg);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  std::string out;
  for (size_t i = 0; i < stack.size(); i++) {
    if (i > 0) out += '/';
    out += stack[i];
  }
  return out;
}

// Directory portion of a path ("domains/ORDERS.md" -> "domains", "README.md" -> "").
static std::string dirname_of(const std::string &path) {
  auto slash = path.rfind('/');
  if (slash == std::string::npos) return "";
  return path.substr(0, slash);
}

bool resolve_doc_ref(const std::string &source_file,
                     const std::string &raw_target,
                     const std::string &link_text,
                     const std::set<std::string> &valid_indexed_paths,
                     DocRefEdge &out) {
  // 1. Strip any #anchor suffix from raw_target FIRST.
  std::string working = raw_target;
  auto hash = working.find('#');
  if (hash != std::string::npos) working = working.substr(0, hash);

  // 2. External link (contains "://"). Keep the ORIGINAL raw_target (with anchor).
  if (working.find("://") != std::string::npos) {
    out.kind = RefKind::External;
    out.target = raw_target;
    out.text = link_text;
    return true;
  }

  // 3. Pure anchor (working empty after stripping) -> drop.
  if (working.empty()) return false;

  // 4. Local path resolution.
  std::string resolved;
  if (working[0] == '/') {
    // absolute-to-project: strip leading '/'
    resolved = working.substr(1);
  } else {
    // relative to dirname(source_file)
    std::string dir = dirname_of(source_file);
    resolved = dir.empty() ? working : (dir + "/" + working);
  }
  std::string normalized = normalize_path(resolved);

  // 5. Keep only if it is an indexed path.
  if (valid_indexed_paths.count(normalized)) {
    out.kind = RefKind::Local;
    out.target = normalized;
    out.text = link_text;
    return true;
  }

  // 6. Dangling / out-of-project -> drop.
  return false;
}
