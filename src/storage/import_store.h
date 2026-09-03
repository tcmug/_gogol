// import_store.h — Per-index import graph storage
#pragma once
#include <map>
#include <string>
#include <vector>

// Import with named symbols
struct ImportInfo {
    std::string module_path;
    std::vector<std::string> symbols; // named imports (empty = import all / side-effect)
};

struct ImportGraph {
    // file → list of imports with symbols
    std::map<std::string, std::vector<ImportInfo>> imports;

    // What does this file import? (returns module paths for backward compat)
    std::vector<std::string> imports_of(const std::string &file) const;
    // What files import this module/file?
    std::vector<std::string> imported_by(const std::string &path) const;
    // What files import a specific symbol from any module?
    std::vector<std::string> files_importing_symbol(const std::string &symbol) const;
};
