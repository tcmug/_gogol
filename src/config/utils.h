// utils.h — Small shared utilities
#pragma once
#include "storage/index_file.h"
#include "config/config.h"
#include <sstream>
#include <string>
#include <vector>

// Split comma-separated string, trim whitespace
inline std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> result;
  std::istringstream iss(s);
  std::string item;
  while (std::getline(iss, item, ',')) {
    size_t start = item.find_first_not_of(" \t");
    size_t end = item.find_last_not_of(" \t");
    if (start != std::string::npos)
      result.push_back(item.substr(start, end - start + 1));
  }
  return result;
}

// Count file and mem entries in an index
struct EntryCounts {
  int file = 0;
  int mem = 0;
};

inline EntryCounts count_entries(const Index &index) {
  EntryCounts c;
  for (auto &e : index.entries) {
    if (e.proto == EntryType::DOC) c.file++;
    else c.mem++;
  }
  return c;
}
