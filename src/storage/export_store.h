// export_store.h — Per-index export symbol storage
#pragma once
#include <map>
#include <string>
#include <vector>

// Exported symbols per file
struct ExportStore {
    // file → list of exported symbol names
    std::map<std::string, std::vector<std::string>> exports;

    std::vector<std::string> exports_of(const std::string &file) const;
};
