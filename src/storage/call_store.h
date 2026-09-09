// call_store.h — Per-index call-graph edge storage
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Stored edge: includes the source file path
struct StoredCallEdge {
    std::string caller;
    std::string callee;
    std::string file; // relative path within index
    uint32_t line;
};

// Call graph for an index
struct CallGraph {
    std::vector<StoredCallEdge> edges;

    // Query helpers
    std::vector<StoredCallEdge> callers_of(const std::string &name) const;
    std::vector<StoredCallEdge> callees_of(const std::string &name) const;

    // File-scoped queries (for disambiguating common names)
    std::vector<StoredCallEdge> callers_of(const std::string &name, const std::string &file) const;
    std::vector<StoredCallEdge> callees_of(const std::string &name, const std::string &file) const;

    // Find which files define a function
    std::vector<std::string> files_defining(const std::string &name) const;
};
