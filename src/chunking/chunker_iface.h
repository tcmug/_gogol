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

// Regex chunker — splits at lines matching a regex pattern.
// Use for structured text formats without tree-sitter support.
// The regex is matched against each line. Matching lines start a new chunk.
// The matched line becomes the heading.
class RegexChunker : public IChunker {
public:
    // exts: supported extensions (e.g., {".graphql", ".gql"})
    // pattern: regex that identifies chunk boundaries (matched per line)
    RegexChunker(std::vector<std::string> exts, const std::string &pattern)
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
