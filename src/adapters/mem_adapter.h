// mem_adapter.h — mem:// protocol adapter
// Manages virtual knowledge entries stored in .mem files.
// Always supports add/remove. Used for r-mode indexes where files aren't writable.
#pragma once
#include "adapters/adapter.h"
#include "storage/mem_store.h"
#include <string>

class MemAdapter : public Adapter {
public:
    MemAdapter(const std::string &name);

    std::string protocol() const override { return "mem"; }

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
    bool supports_add() const override { return true; }
    bool supports_remove() const override { return true; }

    // Access mem store for listing
    std::map<std::string, MemEntry> entries() const;

private:
    std::string name_;
};
