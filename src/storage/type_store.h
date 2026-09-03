// type_store.h — Per-index type hierarchy storage
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Type hierarchy edges
struct StoredTypeEdge {
    std::string name;     // class/interface
    std::string parent;   // what it extends/implements
    std::string kind;     // "extends" or "implements"
    std::string file;
    uint32_t line;
};

struct TypeGraph {
    std::vector<StoredTypeEdge> edges;

    // What does this type extend/implement?
    std::vector<StoredTypeEdge> parents_of(const std::string &name) const;
    // What types extend/implement this?
    std::vector<StoredTypeEdge> children_of(const std::string &name) const;
};
