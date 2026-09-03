#pragma once
#include "chunking/chunker.h"
#include "core/types.h"
#include <string>
#include <vector>

// Returns chunks based on top-level AST nodes (functions, classes, exports)
// Returns empty vector if no grammar available for the extension
std::vector<Chunk> chunk_treesitter(const std::string &content,
                                    const std::string &ext);

// Check if tree-sitter grammar is available for this extension
bool has_treesitter_grammar(const std::string &ext);

// Call-graph edge extracted from AST
struct CallEdge {
    std::string caller;
    std::string callee;
    uint32_t line;
};

// Extract caller→callee edges from source code using tree-sitter
std::vector<CallEdge> extract_calls(const std::string &content,
                                    const std::string &ext);

// Extract per-function metrics from source code
std::vector<FunctionMetrics> extract_metrics(const std::string &content,
                                            const std::string &ext);

// Import edge: file imports module_path with optional named symbols
struct ImportEdge {
    std::string module_path; // raw import path (e.g. "./refund-triggered-entity" or "lib/pricing")
    std::vector<std::string> symbols; // named imports (e.g. {"X", "Y"} from "import { X, Y } from ...")
};

// Extract import paths (with named symbols) from source code using tree-sitter
std::vector<ImportEdge> extract_imports(const std::string &content,
                                       const std::string &ext);

// Exported symbol from a file
struct ExportedSymbol {
    std::string name;     // symbol name
    std::string kind;     // "function", "const", "class", "type", "interface", "enum", "default"
    uint32_t line;        // declaration line
};

// Extract exported symbols from source code
std::vector<ExportedSymbol> extract_exports(const std::string &content,
                                           const std::string &ext);

// Type hierarchy edge: type extends/implements another type
struct TypeEdge {
    std::string name;     // class/interface name
    std::string parent;   // what it extends or implements
    std::string kind;     // "extends" or "implements"
    uint32_t line;        // declaration line
};

// Extract extends/implements relationships
std::vector<TypeEdge> extract_type_edges(const std::string &content,
                                         const std::string &ext);
