#pragma once
#include "config/config.h"
#include "core/loc.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct IndexEntry {
    // On-disk byte: DOC=0 (old FILE), NOTE=1 (old MEMORY). TERM is never stored
    // as an IndexEntry (glossary is a separate store).
    EntryType proto = EntryType::DOC;
    std::string path;  // relative file path (file) or topic (mem)
    std::string chunk; // heading or identifier
    uint32_t line = 0; // 1-based line offset (file), 0 (mem)
    uint32_t end_line = 0; // 1-based end line (0 = unknown)
    uint64_t hash = 0; // change detection hash
    std::vector<float> embedding; // empty until embeddings are loaded
};

struct Index {
    uint32_t dim = 0;
    EmbedPrecision precision = EmbedPrecision::F32;
    std::vector<IndexEntry> entries;

    // Lazy loading: embeddings loaded on demand
    bool emb_loaded = false;
    std::string name_; // for deferred .emb load

    // Load embeddings if not yet loaded. Call before cosine similarity.
    void ensure_embeddings();
};

// Index metadata + embeddings are loaded through the storage backend
// (SqliteBackend::load_index / load_index_counts). The legacy free .meta/.emb
// FILE readers were removed with the SQLite-only migration.
struct IndexInfo {
    std::string name;
    std::string root_path;
};

// Counts per index (dim + doc/note counts). Returned by the backend.
struct IndexCounts { uint32_t dim = 0; uint32_t file_count = 0; uint32_t mem_count = 0; };

// Read-only: index writes go through SqliteBackend (see storage/sqlite_backend.h).
// The legacy .meta + .emb writers were removed from the runtime; test-only
// copies live in tests/legacy_writers.{h,cpp} to exercise the migration read
// path.

// Find index name for a directory path
std::string find_index_for_path(const std::filesystem::path &dir);

// List all indexes
std::vector<IndexInfo> list_indexes();

// Cosine similarity
float cosine_similarity(const std::vector<float> &a,
                        const std::vector<float> &b);

// f16 conversion (exposed for embedding provider)
uint16_t float_to_f16(float f);
float f16_to_float(uint16_t h);
