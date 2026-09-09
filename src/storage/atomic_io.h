// atomic_io.h — Atomic file write: write to .tmp, then rename
#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace fs = std::filesystem;

// Write to path.tmp, then rename to path. Prevents corruption on crash.
// write_fn receives an open ofstream to write to.
inline bool atomic_write(const fs::path &path,
                         std::function<void(std::ofstream &)> write_fn) {
  fs::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec)
    return false;
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return false;
    write_fn(f);
    if (!f.good()) { fs::remove(tmp); return false; }
  }
  std::error_code rename_ec;
  fs::rename(tmp, path, rename_ec);
  return !rename_ec;
}
