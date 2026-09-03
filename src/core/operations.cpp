// operations.cpp — Shared add/rm/get logic used by both CLI and daemon
#include "core/operations.h"
#include "adapters/file_adapter.h"
#include "adapters/mem_adapter.h"
#include "storage/glossary_store.h"
#include "storage/mem_store.h"
#include "storage/storage_backend.h"
#include "config/utils.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

using std::string;
using std::vector;

namespace fs = std::filesystem;

OpResult op_add(EntryType type, const string &index, const string &path,
                const string &content, const string &sources,
                const IndexConfig &config, EmbedProvider &embedder,
                Index &index_data) {
  const string &name = index;
  const string &topic = path;

  if (name.empty())
    return {false, "No index specified"};

  // Term entry -> glossary store.
  if (type == EntryType::TERM) {
    auto backend = open_backend(name);
    auto glossary = backend->load_glossary();
    glossary[topic] = content;
    backend->save_glossary(glossary);
    return {true, "Added: " + format_loc(EntryType::TERM, name, topic)};
  }

  // Note entry -> .mem store (always writable, regardless of mode), indexed as
  // a MEMORY entry.
  if (type == EntryType::NOTE) {
    MemAdapter ma(name);
    vector<string> src_list;
    if (!sources.empty()) src_list = split_csv(sources);
    ma.add(topic, content, src_list);

    string embed_text = topic + ". " + content;
    auto embedding = embedder.embed_document(embed_text);
    if (embedding.empty())
      return {false, "Embedding failed"};

    index_data.entries.erase(
        std::remove_if(index_data.entries.begin(), index_data.entries.end(),
                       [&](auto &e) { return e.proto == EntryType::NOTE && e.path == topic; }),
        index_data.entries.end());
    IndexEntry entry;
    entry.proto = EntryType::NOTE;
    entry.path = topic;
    entry.chunk = topic;
    entry.line = 0;
    entry.hash = static_cast<uint64_t>(std::time(nullptr));
    entry.embedding = std::move(embedding);
    index_data.entries.push_back(std::move(entry));
    if (index_data.dim == 0) index_data.dim = embedder.dim();
    return {true, "Added: " + format_loc(EntryType::NOTE, name, topic)};
  }

  // Doc entry -> a code/doc file under the index's `paths` root. Only allowed
  // on writable (rw) indexes.
  if (!config.is_writable())
    return {false, "Index is read-only"};

  FileAdapter fa(name, config);
  if (!fa.add(topic, content, {}))
    return {false, "Write failed"};

  string embed_text = topic + ". " + content;
  auto embedding = embedder.embed_document(embed_text);
  if (!embedding.empty()) {
    index_data.entries.erase(
        std::remove_if(index_data.entries.begin(), index_data.entries.end(),
                       [&](auto &e) { return e.proto == EntryType::DOC && e.path == topic; }),
        index_data.entries.end());
    IndexEntry entry;
    entry.proto = EntryType::DOC;
    entry.path = topic;
    entry.chunk = topic;
    entry.line = 1;
    entry.hash = 0;
    entry.embedding = std::move(embedding);
    index_data.entries.push_back(std::move(entry));
    if (index_data.dim == 0) index_data.dim = embedder.dim();
  }
  return {true, "Added: " + format_loc(EntryType::DOC, name, topic, 1)};
}

OpResult op_rm(EntryType type, const string &index, const string &path,
               const IndexConfig &config, Index &index_data) {
  const string &name = index;
  const string &topic = path;

  if (name.empty())
    return {false, "No index specified"};

  // Term entry -> glossary store.
  if (type == EntryType::TERM) {
    auto backend = open_backend(name);
    auto glossary = backend->load_glossary();
    if (!glossary.count(topic))
      return {false, "Not found"};
    glossary.erase(topic);
    backend->save_glossary(glossary);
    return {true, "Removed: " + format_loc(EntryType::TERM, name, topic)};
  }

  // Note entry -> remove from the .mem store.
  if (type == EntryType::NOTE) {
    MemAdapter ma(name);
    if (!ma.remove(topic))
      return {false, "Not found"};
    index_data.entries.erase(
        std::remove_if(index_data.entries.begin(), index_data.entries.end(),
                       [&](auto &e) { return e.proto == EntryType::NOTE && e.path == topic; }),
        index_data.entries.end());
    return {true, "Removed: " + format_loc(EntryType::NOTE, name, topic)};
  }

  // Doc entry -> code/doc file. Only on writable (rw) indexes.
  if (!config.is_writable())
    return {false, "Index is read-only"};

  FileAdapter fa(name, config);
  if (!fa.remove(topic))
    return {false, "Not found"};
  return {true, "Removed: " + format_loc(EntryType::DOC, name, topic)};
}

OpResult op_get(EntryType type, const string &index, const string &path,
                const IndexConfig &config, int max_lines) {
  const string &name = index;

  if (name.empty())
    return {false, "No index specified"};

  // Term entry -> glossary store.
  if (type == EntryType::TERM) {
    auto backend = open_backend(name);
    auto glossary = backend->load_glossary();
    auto it = glossary.find(path);
    if (it != glossary.end())
      return {true, it->second};
    return {false, "Not found"};
  }

  // Note entry -> read from the .mem store.
  if (type == EntryType::NOTE) {
    MemAdapter ma(name);
    string content = ma.get_content(path, 0, 0);
    if (!content.empty())
      return {true, content};
    return {false, "Not found"};
  }

  // Doc entry -> file, path may carry :line.
  string file_path;
  uint32_t line = 0;
  split_path_line(path, file_path, line);
  if (line == 0) line = 1;

  FileAdapter fa(name, config);
  int limit = max_lines > 0 ? max_lines : 0;
  string content = fa.get_content(file_path, line, limit);
  if (content.empty())
    return {false, "Cannot read file"};
  return {true, content};
}
