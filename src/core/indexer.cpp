// indexer.cpp — Index pipeline implementation
#include "config/debug.h"
#include "core/indexer.h"
#include "adapters/file_adapter.h"
#include "adapters/mem_adapter.h"
#include "storage/mem_store.h"
#include "storage/call_store.h"
#include "storage/import_store.h"
#include "storage/export_store.h"
#include "storage/type_store.h"
#include "storage/metrics_store.h"
#include "storage/docref_store.h"
#include "storage/glossary_store.h"
#include "storage/index_data.h"
#include "storage/storage_backend.h"
#include "chunking/ts_chunker.h"
#include "chunking/ref_extractor.h"
#include "config/scanner.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

using std::map;
using std::string;
using std::vector;

IndexResult run_index(const string &name, const IndexConfig &cfg,
                      EmbedProvider &embedder, Index &index, bool force,
                      const string &progress_name,
                      IndexProgressFn progress_fn) {
  IndexResult result;
  DBG("run_index: %s (force=%d, existing=%zu entries)", name.c_str(), force, index.entries.size());

  // Ensure embeddings are loaded for preserving unchanged entries
  index.ensure_embeddings();

  FileAdapter file_adapter(name, cfg);
  MemAdapter mem_adapter(name);

  if (file_adapter.root().empty()) return result;

  // All persistence for this index goes through one backend (see
  // storage_backend.h) — SqliteBackend, one <index>.db per index.
  auto backend = open_backend(name);

  // Build hash maps from existing entries
  map<string, uint64_t> file_hashes, mem_hashes;
  for (auto &e : index.entries) {
    if (e.proto == EntryType::DOC) file_hashes[e.path] = e.hash;
    else mem_hashes[e.path] = e.hash;
  }

  // Scan
  DBG("  scanning files..."); auto file_chunks = file_adapter.scan(force, file_hashes); DBG("  scan: %zu changed files", file_chunks.size());
  auto mem_chunks = mem_adapter.scan(force, mem_hashes);

  // Preserve unchanged file entries still in scan results
  vector<IndexEntry> new_entries;
  std::set<string> changed;
  for (auto &c : file_chunks) changed.insert(c.key);
  auto &valid = file_adapter.all_paths();

  for (auto &e : index.entries) {
    if (e.proto == EntryType::DOC && !changed.count(e.path) && valid.count(e.path)) {
      new_entries.push_back(e);
      result.skipped++;
    }
  }

  // Preserve mem entries not being re-embedded
  std::map<std::string, MemEntry> current_mem;
  backend->load_mem(current_mem);
  for (auto &e : index.entries) {
    if (e.proto == EntryType::NOTE) {
      // Only keep MEM entries that still exist in the mem store
      bool re_embed = false;
      for (auto &c : mem_chunks)
        if (c.key == e.path) { re_embed = true; break; }
      if (!re_embed && current_mem.count(e.path))
        new_entries.push_back(e);
      // else: either being re-embedded or orphaned — drop from new_entries
    }
  }

  // Combine pending chunks
  struct Pending {
    EntryType proto; string key; string chunk;
    uint32_t line; uint32_t end_line; uint64_t hash; string text;
  };
  vector<Pending> pending;
  pending.reserve(file_chunks.size() + mem_chunks.size());
  for (auto &c : file_chunks)
    pending.push_back({EntryType::DOC, c.key, c.heading, c.line, c.end_line, c.hash, c.text});
  for (auto &c : mem_chunks)
    pending.push_back({EntryType::NOTE, c.key, c.heading, c.line, 0, c.hash, c.text});

  // Batch embed
  auto gc = load_global_config();
  const int BATCH = gc.batch_size > 0 ? gc.batch_size : 32;
  for (size_t i = 0; i < pending.size(); i += BATCH) {
    size_t end = std::min(i + (size_t)BATCH, pending.size());
    vector<string> texts;
    texts.reserve(end - i);
    for (size_t j = i; j < end; j++) texts.push_back(pending[j].text);

    if (!progress_name.empty())
      fprintf(stderr, "\r\033[K[%s] Embedding %d/%d...",
              progress_name.c_str(), result.embedded + 1, (int)pending.size());
    if (progress_fn)
      progress_fn(result.embedded + 1, (int)pending.size());

    auto embeddings = embedder.embed_documents_batch(texts);
    for (size_t j = 0; j < embeddings.size(); j++) {
      if (!embeddings[j].empty()) {
        IndexEntry entry;
        entry.proto = pending[i + j].proto;
        entry.path = pending[i + j].key;
        entry.chunk = pending[i + j].chunk;
        entry.line = pending[i + j].line;
        entry.end_line = pending[i + j].end_line;
        entry.hash = pending[i + j].hash;
        entry.embedding = std::move(embeddings[j]);
        new_entries.push_back(std::move(entry));
        result.embedded++;
      }
    }
  }
  if (!progress_name.empty()) fprintf(stderr, "\r\033[K");

  // Update index
  index.entries = std::move(new_entries);
  index.dim = embedder.dim();
  index.precision = gc.precision;
  result.total = (int)index.entries.size();

  // Assemble the whole-index payload. Every collection run_index produces is
  // funneled into ONE IndexData and persisted with a single save_all() at the
  // end (atomic on the sqlite backend, identical per-file behavior on the file
  // backend). run_index does NOT own mem/glossary writes — those belong to the
  // add/rm adapter paths — so we load the current on-disk contents and write
  // them back unchanged, preserving that ownership (net no-op for those files).
  IndexData data;
  data.index = index;
  backend->load_mem(data.mem);
  data.glossary = backend->load_glossary();

  // Keyword search is served by the SQLite backend's FTS5 index, which is
  // rebuilt from the entries table inside save_index_locked (see
  // sqlite_backend.cpp). run_index therefore no longer builds a separate
  // in-memory BM25 keyword index.

  // Extract call graph and import graph incrementally
  {
    CallGraph &graph = data.calls;
    ImportGraph &igraph = data.imports;
    MetricsStore &mstore = data.metrics;
    ExportStore &estore = data.exports;
    TypeGraph &tgraph = data.types;
    if (!force) {
      graph = backend->load_call_graph();
      igraph = backend->load_import_graph();
      mstore = backend->load_metrics();
    }

    // Remove edges from files that changed or no longer exist
    graph.edges.erase(
        std::remove_if(graph.edges.begin(), graph.edges.end(),
                       [&](auto &e) { return changed.count(e.file) || !valid.count(e.file); }),
        graph.edges.end());
    for (auto &path : changed) igraph.imports.erase(path);
    mstore.entries.erase(
        std::remove_if(mstore.entries.begin(), mstore.entries.end(),
                       [&](auto &m) { return changed.count(m.file) || !valid.count(m.file); }),
        mstore.entries.end());
    for (auto it = igraph.imports.begin(); it != igraph.imports.end(); )
      if (!valid.count(it->first)) it = igraph.imports.erase(it); else ++it;

    // Re-extract from changed files only
    for (auto &path : changed) {
      string ext = fs::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (!has_treesitter_grammar(ext)) continue;

      string abs = (path[0] == '/') ? path : (fs::path(file_adapter.root()) / path).string();
      std::ifstream f(abs);
      if (!f) continue;
      std::ostringstream ss;
      ss << f.rdbuf();
      string content = ss.str();
      if (content.empty()) continue;

      auto edges = extract_calls(content, ext);
      for (auto &e : edges)
        graph.edges.push_back({e.caller, e.callee, path, e.line});

      auto imports = extract_imports(content, ext);
      for (auto &imp : imports)
        igraph.imports[path].push_back({imp.module_path, imp.symbols});

      auto file_metrics = extract_metrics(content, ext);
      for (auto &m : file_metrics) {
        m.file = path;
        mstore.entries.push_back(m);
      }

      auto file_exports = extract_exports(content, ext);
      for (auto &e : file_exports)
        estore.exports[path].push_back(e.name);

      auto type_edges = extract_type_edges(content, ext);
      for (auto &e : type_edges)
        tgraph.edges.push_back({e.name, e.parent, e.kind, path, e.line});
    }
    DBG("  call graph: %zu edges", graph.edges.size());
    DBG("  import graph: %zu files", igraph.imports.size());
    DBG("  metrics: %zu functions", mstore.entries.size());
    DBG("  exports: %zu files", estore.exports.size());
    DBG("  type graph: %zu edges", tgraph.edges.size());
  }

  // Extract document reference graph incrementally (markdown links)
  {
    DocRefGraph &drefs = data.docrefs;
    if (!force) drefs = backend->load_docref_graph();

    // Remove edges from files that changed or no longer exist
    for (auto &path : changed) drefs.refs.erase(path);
    for (auto it = drefs.refs.begin(); it != drefs.refs.end(); )
      if (!valid.count(it->first)) it = drefs.refs.erase(it); else ++it;

    // Set of valid indexed paths for resolution
    std::set<std::string> valid_set(valid.begin(), valid.end());

    MarkdownLinkExtractor extractor;

    // Re-extract from changed markdown files only
    for (auto &path : changed) {
      string ext = fs::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (!extractor.supports(ext)) continue;

      string abs = (path[0] == '/') ? path : (fs::path(file_adapter.root()) / path).string();
      std::ifstream f(abs);
      if (!f) continue;
      std::ostringstream ss;
      ss << f.rdbuf();
      string content = ss.str();
      if (content.empty()) continue;

      auto refs = extractor.extract(content);
      for (auto &r : refs) {
        DocRefEdge edge;
        if (resolve_doc_ref(path, r.target, r.text, valid_set, edge))
          drefs.refs[path].push_back(edge);
      }
    }
    DBG("  docref graph: %zu files", drefs.refs.size());
  }

  // Persist the whole index in one call — no per-collection orchestration.
  backend->save_all(file_adapter.root(), data);

  return result;
}
