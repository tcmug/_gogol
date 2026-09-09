#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Returns pairs of (relative_path, change_hash)
// change_hash is based on mtime+size (fast, no file read)
std::vector<std::pair<std::string, uint64_t>>
scan_md_files(const std::filesystem::path &root,
              const std::vector<std::string> &extensions = {".md"});

std::string read_file(const std::string &path);
