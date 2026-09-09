// ref_extractor.h — pluggable extraction of document references (links)
#pragma once
#include <memory>
#include <string>
#include <vector>

// A reference found in a document (e.g. a markdown link).
struct DocRef {
    std::string text;    // link text (the human title)
    std::string target;  // raw target as written, e.g. "./domains/ORDERS.md" or "https://..."
};

// Extracts references from document content for a given file type.
class IRefExtractor {
public:
    virtual ~IRefExtractor() = default;

    // Can this extractor handle the given file extension?
    virtual bool supports(const std::string &ext) const = 0;

    // Extract all references found in the content.
    virtual std::vector<DocRef> extract(const std::string &content) const = 0;
};

// Markdown link extractor — pulls [text](target) links.
// Skips images (![...](...)) and links inside fenced code blocks.
// Returns everything it finds (does not filter anchors/urls — that is
// resolution's job).
class MarkdownLinkExtractor : public IRefExtractor {
public:
    bool supports(const std::string &ext) const override; // true for .md, .mdx
    std::vector<DocRef> extract(const std::string &content) const override;
};

// Get ordered list of ref extractors (tried in order, first match wins).
std::vector<std::unique_ptr<IRefExtractor>> default_ref_extractors();
