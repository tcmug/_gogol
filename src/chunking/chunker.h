#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Chunk {
    std::string heading; // section heading (or "" for window chunks)
    uint32_t line;       // 1-based line offset
    uint32_t end_line;   // 1-based end line (0 = unknown)
    std::string content; // chunk text for embedding
};

// Markdown: split by h1/h2 headings, merge tiny sections
std::vector<Chunk> chunk_markdown(const std::string &text);

// Generic: fixed character window with overlap
std::vector<Chunk> chunk_window(const std::string &text, int window = 2000,
                                int overlap = 200);
