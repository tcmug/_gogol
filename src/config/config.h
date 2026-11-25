#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Synthesized per-repo config for the ingest pipeline (refresh_repo builds one from
// the git identity; FileAdapter reads paths+extensions). NOT parsed from a config file
// anymore — the per-[index] section model was removed with zero-config auto-indexing.
struct IndexConfig {
    std::string name;                    // index identity (becomes entries.idx)
    std::vector<std::string> paths;      // scan roots (FileAdapter)
    std::vector<std::string> extensions; // with dots: ".md", ".ts"
};

// Global config (top-level keys outside sections)
struct GlobalConfig {
    std::string model; // default model path (empty = use GOGOL_MODEL / ~/.gogol/model)
    // Retrieval handshake prefixes — a per-MODEL property. std::nullopt = AUTO-SELECT
    // from the model's architecture (Embedder::default_prefixes_for_arch); a value
    // OVERRIDES for an unknown/custom model. Wrong prefix => degenerate ranking.
    std::optional<std::string> query_prefix;
    std::optional<std::string> document_prefix;

    // [mcp] section — the MCP server is opt-in (off unless enabled). `gogol mcp` is a
    // local stdio tool, so there is no per-tool gate; this single switch starts it.
    bool mcp_enabled = false;      // [mcp] enabled = true|false (default false)
    int http_port = 0;             // [mcp] http_port = N (0 = serve disabled)
    std::string http_host = "127.0.0.1"; // [mcp] http_host = ... (default loopback)

    // Daemon transport (shared warm instance, sole SQLite writer). The daemon can serve
    // a local UNIX SOCKET (thin stdio shims connect to it — no TCP, no LAN exposure) AND
    // optionally HTTP (above) at the same time; a deployment enables whichever it needs.
    bool socket_enabled = false;   // [mcp] socket = on|off (default off)
    std::string socket_path;       // [mcp] socket_path = ... (default: gogol_home/daemon.sock)
    int idle_timeout_secs = 0;     // [mcp] idle_timeout_secs = N (0 = never idle-exit)

    // Agent messaging over MCP (combadge bus): advertises `listen`/`send` in tools/list and
    // adds an unread-count hint to results for callers that pass `me`. Off = invisible.
    bool messaging = false;        // [mcp] messaging = true|false (default false)
};

GlobalConfig load_global_config();

// --- gogol home directory (single source; overridable) ---
// The convention home for all gogol state (model symlink, cache.db, notes.db).
// Resolution: $GOGOL_HOME if set, else $HOME/.gogol. This is the ONE place the
// ".gogol" path is derived — every consumer builds on gogol_home() rather than
// re-deriving getenv("HOME")+"/.gogol" (which was duplicated across ~8 sites).
std::string gogol_home();
// Derived paths under gogol_home():
std::string cache_db_path();  // gogol_home()/cache.db — derived/disposable store
std::string notes_db_path();  // gogol_home()/notes.db — disposable-but-not-re-derivable notes
std::string messages_db_path(); // gogol_home()/messages.db — combadge bus queue (ADR-003)
std::string model_path();     // gogol_home()/model (symlink) unless GOGOL_MODEL overrides
std::string config_file_path(); // gogol_home()/config — the INI file (GOGOL_HOME isolates it)

// Expand a leading ~ or ~/ to $HOME (the ONE tilde-expansion helper; callers in
// config/git_identity/refresh all use it). A bare "~" and "~/x" both expand; anything
// else is returned unchanged. No-op if $HOME is unset.
std::string expand_tilde(const std::string &path);

// Load [chunkers] section: extensions (comma-sep) → regex pattern
struct ChunkerDef {
    std::vector<std::string> extensions; // with dots: ".graphql", ".gql"
    std::string pattern;                 // regex
};
std::vector<ChunkerDef> load_chunker_config();

// --- Automatic daily backup of notes.db (Fibonacci retention) ---
// cache.db is derived/rebuildable, so it is NOT backed up. notes.db is the
// LLM-authored content of record — NOT rebuildable — so it alone gets backed up.
//
// One backup per calendar day of notes.db (and its -wal/-shm sidecars) into
// gogol_home()/backups/YYYY-MM-DD/. At most one backup per day — the dated
// directory name itself is the guard (re-running the same day is a no-op).
//
// Retention follows a Fibonacci schedule up to 13 days: backups are kept for ages
// {1,2,3,5,8,13} days. A backup older than 13 days is deleted; a backup whose age
// is not in the set is also deleted (so only the 6 Fibonacci-aged snapshots survive
// at steady state). Dense recent coverage + sparse older snapshots.
//
// Best-effort, non-throwing: any copy/IO error is swallowed (a backup failure must
// never block a gogol invocation). Failures land on stderr only when GOGOL_DEBUG.
// Idempotent: safe to call on every CLI/MCP startup. Skips silently if notes.db
// doesn't exist yet (fresh install) or today's backup already exists.
void maybe_backup_dbs();
