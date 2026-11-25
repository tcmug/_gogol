// file_adapter.h — scans a directory tree and chunks files via the IChunker chain.
#pragma once
#include "adapters/adapter.h"
#include "chunking/chunker_iface.h"
#include "config/config.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Field separator for enriched text: ␞ (U+241E, "symbol for record separator").
// Printable Unicode character that never appears in code/docs. Format:
//   {context}␞{content}␞{keywords}
constexpr const char* kFieldSep = "\xE2\x90\x9E";

class FileAdapter {
public:
    explicit FileAdapter(const std::string &name, const IndexConfig &config);

    std::vector<AdapterChunk>
    scan(bool force,
       const std::map<std::string, uint64_t> &existing_hashes);

    const std::string &root() const { return root_; }

    // Enable git-identity nested-repo-boundary skip during scan. When set (to the
    // repo toplevel), scan() will not descend into subdirs that are the top of a
    // DIFFERENT git repo. Empty (default) => plain recursive scan (config-path
    // behavior unchanged). Forwarded to scan_md_files as its repo_root argument.
    void set_repo_root(const std::string &repo_root) { repo_root_ = repo_root; }

    // All valid file paths found during last scan (including skipped unchanged)
    const std::set<std::string> &all_paths() const { return all_paths_; }

private:
    std::string name_;
    IndexConfig config_;
    std::string root_;
    std::string repo_root_;   // git-identity nested-repo skip anchor (empty = off)
    std::vector<std::unique_ptr<IChunker>> chunkers_;
    std::set<std::string> all_paths_;
};
