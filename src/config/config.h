#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class IndexMode { R, RW };

struct IndexConfig {
    std::string name;                    // index name (section header)
    std::vector<std::string> paths;
    std::vector<std::string> extensions; // with dots: ".md", ".ts"
    IndexMode mode = IndexMode::R;
    std::string model; // GGUF model path (optional, overrides global)
    std::string memory; // memory dir (optional; default ~/.gogol/memory/<name>)

    // Whether gogol may write files under `paths`. The memory dir is always
    // writable regardless of this (see memory_dir()).
    bool is_writable() const { return mode == IndexMode::RW; }
    // All configured indexes are searched. (Kept for call-site clarity.)
    bool is_indexed() const { return true; }

    // Resolved memory directory: explicit `memory` or default under ~/.gogol.
    // The memory dir is always writable (holds note/term entries),
    // independent of `mode` (which only governs writes to `paths`).
    std::string memory_dir() const;
};

// Global config (top-level keys outside sections)
enum class EmbedPrecision : uint8_t { F32 = 0, F16 = 1 };

struct GlobalConfig {
    std::string model; // default model path
    std::string tcp;   // TCP listen address (empty = disabled)
    int batch_size = 32; // embedding batch size
    bool watch = false;  // enable filesystem watching in daemon
    int watch_debounce_ms = 2000; // debounce interval for watch events
    EmbedPrecision precision = EmbedPrecision::F32;

    // [mcp] section — the MCP server is opt-in (off unless enabled).
    bool mcp_enabled = false;      // [mcp] enabled = true|false (default false)
    std::string mcp_tools = "read"; // [mcp] tools = read|read-write (default read)

    // Whether write tools should be exposed / allowed over MCP.
    bool mcp_read_write() const { return mcp_tools == "read-write"; }
};

// Load ~/.gogol/config, returns map of name → config
std::map<std::string, IndexConfig> load_config();
GlobalConfig load_global_config();

// Load [keys] section: name → 32-byte key (from hex)
std::map<std::string, std::vector<uint8_t>> load_keys();

// Load [chunkers] section: extensions (comma-sep) → regex pattern
struct ChunkerDef {
    std::vector<std::string> extensions; // with dots: ".graphql", ".gql"
    std::string pattern;                 // regex
};
std::vector<ChunkerDef> load_chunker_config();
