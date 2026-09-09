// format.h — Output formatting utilities
#pragma once
#include "config/config.h"
#include "core/searcher.h"
#include <map>
#include <string>

// Output format mode
enum class OutputFormat { DEFAULT, AGENT };

// Path display mode
enum class PathMode { FULL, ABS, SHORT };

// Detect output format from env/isatty
OutputFormat detect_format();

// Global format (set once at startup)
extern OutputFormat g_format;
extern PathMode g_path_mode;

// Staleness suffix: "" for ok, "~" or " (stale)" depending on format
std::string format_stale_suffix(const std::string &status);

// Format a full location string using global path mode
// end_line=0 means unknown (show start only)
std::string format_location(const std::string &index_name, const std::string &rel_path,
                            uint32_t line, const std::string &chunk,
                            const std::map<std::string, IndexConfig> &configs,
                            uint32_t end_line = 0);

// Format a doc reference in the type-prefixed grammar: "doc <index> <path>:<line> § <chunk>"
// (path-mode aware; round-trips into `get doc <index> <path>`). chunk optional.
std::string format_doc_ref(const std::string &index_name, const std::string &rel_path,
                           uint32_t line, const std::string &chunk,
                           const std::map<std::string, IndexConfig> &configs);

// Shorthand: format_location without chunk
std::string format_path(const std::string &index_name, const std::string &rel_path,
                        uint32_t line,
                        const std::map<std::string, IndexConfig> &configs);

// Format a single query result line to stdout.
// `extra` is appended to the location line (after the stale suffix, before the
// newline) — used e.g. for doc-reference counts. Defaults to none.
void print_result(const SearchResult &r, bool scores,
                  const std::map<std::string, IndexConfig> &configs,
                  const std::string &extra = "");

// Format index summary line
std::string format_index_summary(const std::string &name, const std::string &mode,
                                 int file_count, int mem_count, const std::string &path);

// Convert IndexMode enum to display string
inline const char *mode_string(IndexMode m) {
    switch (m) {
    case IndexMode::RW: return "rw";
    default: return "r";
    }
}
