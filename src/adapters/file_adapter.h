// file_adapter.h — file:// protocol adapter
// Scans directories, chunks files via pluggable IChunker chain, checks staleness via stat.
// Supports add/remove only for writable indexes (rw/git/w modes).
#pragma once
#include "adapters/adapter.h"
#include "chunking/chunker_iface.h"
#include "config/config.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class FileAdapter : public Adapter {
public:
    FileAdapter(const std::string &name, const IndexConfig &config);

    std::string protocol() const override { return "file"; }

    std::vector<AdapterChunk>
    scan(bool force,
       const std::map<std::string, uint64_t> &existing_hashes) override;

    EntryStatus check_stale(const std::string &key,
                          uint64_t stored_hash) override;

    std::string get_content(const std::string &key, uint32_t line,
                          int max_lines) override;

    bool add(const std::string &key, const std::string &content,
           const std::vector<std::string> &sources) override;
    bool remove(const std::string &key) override;
    bool supports_add() const override { return writable_; }
    bool supports_remove() const override { return writable_; }

    const std::string &root() const { return root_; }

    // All valid file paths found during last scan (including skipped unchanged)
    const std::set<std::string> &all_paths() const { return all_paths_; }

private:
    std::string name_;
    IndexConfig config_;
    std::string root_;
    bool writable_;
    std::vector<std::unique_ptr<IChunker>> chunkers_;
    std::set<std::string> all_paths_;
    std::map<std::string, std::string> glossary_;
};
