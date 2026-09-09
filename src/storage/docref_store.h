// docref_store.h — Per-index document reference graph storage
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

enum class RefKind { Local, External };

struct DocRefEdge {
    RefKind kind;
    std::string target; // resolved index-relative path (Local) or raw URL (External)
    std::string text;   // link text / title
};

struct DocRefGraph {
    // source_file → list of reference edges
    std::map<std::string, std::vector<DocRefEdge>> refs;

    // Forward: all reference edges from this file (all kinds).
    std::vector<DocRefEdge> references_of(const std::string &file) const;
    // Reverse: files that reference this file via LOCAL edges only (target == file).
    std::vector<std::string> referenced_by(const std::string &file) const;
};

// Read-only: writes go through SqliteBackend.

// Resolve a raw DocRef found in source_file into a DocRefEdge.
// Returns true and fills `out` if the ref should be kept; false to drop.
bool resolve_doc_ref(const std::string &source_file,
                     const std::string &raw_target,
                     const std::string &link_text,
                     const std::set<std::string> &valid_indexed_paths,
                     DocRefEdge &out);
