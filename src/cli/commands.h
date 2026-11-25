// commands.h — CLI command handlers
#pragma once
#include <string>
#include <vector>

// Args carries the parsed CLI options for the live subcommands.
struct Args {
    std::string model;       // embedding model path
    std::string query_text;  // recall <text> / links <node> / delve <symbol> / literal <pattern>
    std::string content;     // remember <content> / index <dir>
    std::string uri;         // get <id> / forget <id>
    int top_k = 5;           // recall --top / literal --limit
    std::string link_kind;   // links --kind (call|import|docref)
    std::string link_dir;    // links --direction (out|in|both)
    int link_depth = 1;      // links --depth (transitive hops)
    std::string type_filter; // recall --type (note|doc|code)
    std::string scope;       // remember --scope / recall query scope (default CWD git toplevel)
    std::string consolidate_id; // remember --consolidate <id>: forget this note, atomically, with the put
    bool scope_set = false;  // whether --scope was explicitly given (distinguish "" global from unset)
    std::string metrics_filter; // metrics [file] path/file substring filter
    int metrics_limit = 20;  // metrics --limit
    bool overlay_stop = false;   // overlay --stop: clear the overlay flag instead of setting it
    std::string ref;         // recall --ref <root-sha>: portable repo identity (read-only lookup)
};

// Top-level verbs (CLI/MCP-parallel): fetch by id (GET), semantic search (QUERY),
// literal phrase search (LITERAL), deep-dive a symbol (DELVE), graph navigation (LINKS),
// complexity ranking (METRICS), author a note (ADD_NOTE), remove a note (FORGET), find
// related entries (RELATED), index a repo (INDEX_PATH), store stats pointer (STATS).
// (Replacing a note is ADD_NOTE with consolidate_id set — no separate action.)
enum class StoreAction { STATS, QUERY, LITERAL, ADD_NOTE, INDEX_PATH, FORGET, LINKS, GET, DELVE, METRICS, RELATED, OVERLAY };
int cmd_store(const Args& args, StoreAction action);
