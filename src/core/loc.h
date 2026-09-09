// loc.h — Entry types and location formatting.
//
// gogol has three entry types, selected explicitly by command (no sigils):
//   doc  — a file chunk under an index's paths   (e.g. `doc web src/utils.ts:42`)
//   note — a memory note in the .mem store        (e.g. `note team auth/flows`)
//   term — a glossary term                        (e.g. `term team MFC`)
//
// A location is a (type, index, path) triple. For doc, `path` may carry a
// trailing :line (e.g. `src/utils.ts:42`).
#pragma once
#include <cstdint>
#include <string>

enum class EntryType { DOC, NOTE, TERM };

// Parse a type token ("doc"/"note"/"term"). Returns true on success.
bool parse_entry_type(const std::string &s, EntryType &out);

// Type token as string ("doc"/"note"/"term").
const char *entry_type_str(EntryType t);

struct ParsedLoc {
    EntryType type = EntryType::DOC;
    std::string index; // index name
    std::string path;  // file path (doc) / topic (note) / term (term)
    uint32_t line = 0; // 1-based line for doc path:line (0 = none)
};

// Split a doc path of the form "src/utils.ts:42" into path + line.
// For a plain path (no trailing :digits), line stays 0.
void split_path_line(const std::string &in, std::string &path, uint32_t &line);

// Format a location for display / agent output. Round-trips into the
// corresponding `get <type> <index> <path>` command.
//   doc:  "doc <index> <path>:<line> § <chunk>"  (line/chunk omitted if unset)
//   note: "note <index> <topic>"
//   term: "term <index> <term>"
std::string format_loc(EntryType type, const std::string &index_name,
                       const std::string &path, uint32_t line = 0,
                       const std::string &chunk = "");
