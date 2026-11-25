// chunker_iface.h — Chunker interface for pluggable file splitting strategies
#pragma once
#include "chunking/chunker.h"
#include <memory>
#include <string>
#include <vector>

class IChunker {
public:
    virtual ~IChunker() = default;

    // Can this chunker handle the given file extension?
    virtual bool supports(const std::string &ext) const = 0;

    // Chunk file content into embeddable pieces
    virtual std::vector<Chunk> chunk(const std::string &content,
                                   const std::string &path) = 0;
};

// Tree-sitter AST chunker — splits at top-level declarations
class TreeSitterChunker : public IChunker {
public:
    bool supports(const std::string &ext) const override;
    std::vector<Chunk> chunk(const std::string &content,
                           const std::string &path) override;
};

// Window chunker — fixed-size character window with overlap (fallback)
class WindowChunker : public IChunker {
public:
    bool supports(const std::string &) const override { return true; } // universal fallback
    std::vector<Chunk> chunk(const std::string &content,
                           const std::string &path) override;
};

// RST chunker — splits reStructuredText at real section headers only.
// A section header is a non-blank title line immediately followed by an
// underline of a single repeated char (=, -, ~, ^, ") of length >= 3.
// Standalone underlines (transitions/table separators) do NOT split, so
// Property/Method tables stay intact. The title (not the underline) is the
// heading. Files with no title+underline pairs yield one chunk and the
// universal WindowChunker fallback then applies at ingest.
class RstChunker : public IChunker {
public:
    bool supports(const std::string &ext) const override;
    std::vector<Chunk> chunk(const std::string &content,
                           const std::string &path) override;
};

// Regex chunker — splits at lines matching a regex pattern.
// Use for structured text formats without tree-sitter support.
// The regex is matched against each line. Matching lines start a new chunk.
// The matched line becomes the heading.
class RegexChunker : public IChunker {
public:
    // exts: supported extensions (e.g., {".graphql", ".gql"})
    // pattern: regex that identifies chunk boundaries (matched per line)
    explicit RegexChunker(std::vector<std::string> exts, const std::string &pattern)
            : exts_(std::move(exts)), pattern_(pattern) {}

    bool supports(const std::string &ext) const override;
    std::vector<Chunk> chunk(const std::string &content,
                           const std::string &path) override;

private:
    std::vector<std::string> exts_;
    std::string pattern_;
};

// Get ordered list of chunkers (tried in order, first match wins)
std::vector<std::unique_ptr<IChunker>> default_chunkers();

// The default set of file extensions to index when no explicit `ext=` list is
// configured (the configless / git-identity path). This is the ONE place the
// built-in extension set is defined: it is derived from the chunkers gogol
// actually has — the tree-sitter grammars (has_treesitter_grammar) plus the
// built-in RegexChunker formats (markdown, GraphQL, YAML, SQL). Extensions are
// lowercase, with a leading dot (".ts", ".md"). The universal WindowChunker is
// intentionally NOT enumerated — it matches everything, so including it would
// index every file type; the default set is code + structured-doc formats only.
std::vector<std::string> default_index_extensions();
