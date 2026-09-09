// adapter.h — Base interface for protocol adapters (file://, mem://, etc.)
// Each adapter handles scanning, staleness, content retrieval, and add/remove
// for its protocol. The indexer is protocol-agnostic.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// A chunk ready for embedding
struct AdapterChunk {
  std::string key;      // unique key within this adapter (relative path, topic, etc.)
  std::string heading;  // chunk heading (for display)
  uint32_t line = 0;    // line offset (files only, 0 for mem)
  uint32_t end_line = 0; // end line (0 = unknown)
  uint64_t hash = 0;    // change-detection hash (0 = always re-embed)
  std::string text;     // text to embed (with context prefix)
};

// Staleness result
enum class EntryStatus { OK, STALE, MISSING };

// Base adapter interface — each protocol (file://, mem://) implements this
class Adapter {
public:
  virtual ~Adapter() = default;

  // Return the protocol name (e.g., "file", "mem")
  virtual std::string protocol() const = 0;

  // Produce chunks to embed. Called by `gogol index`.
  // Returns all chunks that need embedding (respects force flag).
  // existing_hashes: key → hash of previously embedded chunks (for skip detection)
  virtual std::vector<AdapterChunk>
  scan(bool force,
       const std::map<std::string, uint64_t> &existing_hashes) = 0;

  // Check staleness of a single entry
  virtual EntryStatus check_stale(const std::string &key,
                                  uint64_t stored_hash) = 0;

  // Retrieve content for display (for --show in query, or `gogol get`)
  // Returns content string, empty on failure.
  virtual std::string get_content(const std::string &key, uint32_t line,
                                  int max_lines) = 0;

  // Add a new entry. Returns true on success.
  // Not all adapters support this (file adapter in r-mode doesn't).
  virtual bool add(const std::string &key, const std::string &content,
                   const std::vector<std::string> &sources) {
    (void)key;
    (void)content;
    (void)sources;
    return false;
  }

  // Remove an entry. Returns true on success.
  virtual bool remove(const std::string &key) {
    (void)key;
    return false;
  }

  // Whether this adapter supports add/remove
  virtual bool supports_add() const { return false; }
  virtual bool supports_remove() const { return false; }
};
