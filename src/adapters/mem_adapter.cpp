#include "adapters/mem_adapter.h"
#include "storage/storage_backend.h"
#include <ctime>
#include <sstream>

// Mem persistence routes through the StorageBackend (open_backend) rather than
// the ::load_mem_store/::save_mem_store free functions directly, so a mem note
// lands in the same backend (file/sqlite) as the rest of the index. This is
// safe from an include standpoint: storage_backend.h pulls only store headers,
// never adapter headers, so there is no cycle.
MemAdapter::MemAdapter(const std::string &name) : name_(name) {}

std::vector<AdapterChunk>
MemAdapter::scan(bool force,
                 const std::map<std::string, uint64_t> &existing_hashes) {
  std::vector<AdapterChunk> result;
  std::map<std::string, MemEntry> store;
  if (!open_backend(name_)->load_mem(store)) return result;

  for (auto &[topic, entry] : store) {
    // Use timestamp as hash for change detection
    uint64_t hash = static_cast<uint64_t>(entry.timestamp);
    if (!force) {
      auto it = existing_hashes.find(topic);
      if (it != existing_hashes.end() && it->second == hash)
        continue;
    }
    std::string embed_text = topic + ". " + entry.content;
    result.push_back({topic, topic, 0, 0, hash, embed_text});
  }

  return result;
}

EntryStatus MemAdapter::check_stale(const std::string &key,
                                    uint64_t stored_hash) {
  std::map<std::string, MemEntry> store;
  if (!open_backend(name_)->load_mem(store)) return EntryStatus::MISSING;
  auto it = store.find(key);
  if (it == store.end())
    return EntryStatus::MISSING;
  uint64_t current_hash = static_cast<uint64_t>(it->second.timestamp);
  return (current_hash != stored_hash) ? EntryStatus::STALE : EntryStatus::OK;
}

std::string MemAdapter::get_content(const std::string &key, uint32_t line,
                                    int max_lines) {
  (void)line;
  std::map<std::string, MemEntry> store;
  if (!open_backend(name_)->load_mem(store)) return {};
  auto it = store.find(key);
  if (it == store.end())
    return {};

  if (max_lines <= 0)
    return it->second.content;

  // Truncate to max_lines
  std::istringstream iss(it->second.content);
  std::string l, result;
  int n = 0;
  while (std::getline(iss, l) && n < max_lines) {
    if (!result.empty())
      result += "\n";
    result += l;
    n++;
  }
  return result;
}

bool MemAdapter::add(const std::string &key, const std::string &content,
                     const std::vector<std::string> &sources) {
  std::map<std::string, MemEntry> store;
  auto backend = open_backend(name_);
  if (!backend->load_mem(store)) return false;
  MemEntry entry;
  entry.content = content;
  entry.timestamp = std::time(nullptr);
  entry.sources = sources;
  store[key] = entry;
  backend->save_mem(store);
  return true;
}

bool MemAdapter::remove(const std::string &key) {
  std::map<std::string, MemEntry> store;
  auto backend = open_backend(name_);
  if (!backend->load_mem(store)) return false;
  if (store.find(key) == store.end())
    return false;
  store.erase(key);
  backend->save_mem(store);
  return true;
}

std::map<std::string, MemEntry> MemAdapter::entries() const {
  std::map<std::string, MemEntry> store;
  open_backend(name_)->load_mem(store);
  return store;
}
