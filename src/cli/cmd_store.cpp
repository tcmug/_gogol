// cmd_store.cpp — v2 memory store CLI: stats, cross-index query, notes, graph, metrics.
#include <optional>
#include <functional>
#include <cmath>
#include <cctype>
#include <unordered_map>
// ~/.gogol/cache.db and notes.db form the unified store. Configless + self-healing: fresh/changed
// files are indexed on demand (refresh_repo) at query time; there is no manual
// sync/cutover/categorize path anymore (those legacy maintenance flags were dropped).
#include "cli/commands.h"

#include "embedding/embed_provider.h"
#include "store/store.h"
#include "adapters/file_adapter.h"
#include "chunking/ts_chunker.h"
#include "chunking/chunker_iface.h"   // default_index_extensions
#include "config/config.h"
#include "config/scanner.h"
#include "cli/git_identity.h"                // gogol::resolve_git_identity (git-identity index)
#include "core/refresh.h"              // gogol::refresh_repo, IngestResult
#include "core/result_formatters.h"  // gogol::format_recall_results
#include "storage/index_file.h" // f16_to_float

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <set>
#include <map>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

// --- Progress spinner for indexing (stderr so it doesn't break stdout JSON) ---
namespace {
struct ProgressSpinner {
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point last_update;
    bool is_tty = false;
    static constexpr const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    int frame = 0;

    ProgressSpinner() : start(std::chrono::steady_clock::now()), last_update(start), is_tty(isatty(fileno(stderr))) {}

    bool update(int current, int total, const std::string &file) {
        if (!is_tty || total == 0) return true;
        
        // Throttle: update at most every 80ms for smooth animation
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count() < 80)
            return true;
        last_update = now;
        
        int pct = (current * 100) / total;
        
        // ETA calculation
        double elapsed_s = std::chrono::duration<double>(now - start).count();
        std::string eta_str;
        if (current > 0 && elapsed_s > 0.5) {
            double rate = current / elapsed_s;
            int remaining = total - current;
            int eta_s = (rate > 0) ? (int)(remaining / rate) : 0;
            if (eta_s < 60) {
                char buf[16]; snprintf(buf, sizeof(buf), "%ds", eta_s);
                eta_str = buf;
            } else {
                char buf[16]; snprintf(buf, sizeof(buf), "%dm%02ds", eta_s/60, eta_s%60);
                eta_str = buf;
            }
        }
        
        // Get just the filename (not full path)
        std::string fname = file;
        auto pos = fname.rfind('/');
        if (pos != std::string::npos) fname = fname.substr(pos + 1);
        if (fname.size() > 25) fname = fname.substr(0, 22) + "...";
        
        frame = (frame + 1) % 10;
        fprintf(stderr, "\r%s Embedding %3d%%", frames[frame], pct);
        if (!eta_str.empty()) fprintf(stderr, " ETA %s", eta_str.c_str());
        fprintf(stderr, "  %s", fname.c_str());
        // Pad with spaces to clear previous longer lines
        fprintf(stderr, "                    ");
        fprintf(stderr, "\r");
        fflush(stderr);
        return true;
    }

    void done(int total) {
        if (!is_tty) return;
        auto now = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(now - start).count();
        fprintf(stderr, "\r✓ Embedded %d chunks in %.1fs                              \n", total, elapsed_s);
        fflush(stderr);
    }

    void clear() {
        if (!is_tty) return;
        fprintf(stderr, "\r                                                          \r");
        fflush(stderr);
    }
};
} // anon namespace

// Build an embedder using the model's configured retrieval prefixes (the per-model
// query/document handshake). Reads GlobalConfig so a non-nomic model gets its correct
// prefixes; defaults (in GlobalConfig) are nomic's, so existing setups are unchanged.
static std::string json_escape(const std::string &s);  // fwd-decl (defined below)
static std::unique_ptr<EmbedProvider> get_embedder_cfg(const std::string &model_path) {
    auto gc = load_global_config();
    return get_embedder(model_path, gc.query_prefix, gc.document_prefix);
}

static std::string cache_db_path_cli() {
    fs::create_directories(gogol_home());
    return cache_db_path();
}

static std::string notes_db_path_cli() {
    fs::create_directories(gogol_home());
    return notes_db_path();
}

// IngestResult + refresh_repo live in core/refresh.{h,cpp} (shared with the MCP
// server + tests). Alias so the indexing code below reads unchanged.
using gogol::IngestResult;

// ONE lazy-index chokepoint for every read command (recall/links/delve/metrics/
// literal/related). CLI = HALT-UNTIL-INDEXED: before answering, ensure the target
// repo's new/changed files are indexed, blocking as long as needed (a one-shot CLI
// can wait — correctness over latency, NO budget cap).
//
// Target repo = explicit --scope if given, else the CWD's git toplevel. Not in a git
// repo (e.g. a non-git parent dir) => nothing to refresh here; a parent-dir union over
// already-indexed children is handled at query time by the scope resolver. --ref is a
// read-only identity lookup with no path to scan => skip refresh.
//
// The caller's query `store` is READ-ONLY, so this uses its OWN writable handle
// (committed + closed before the read). Centralising this here is the fix for the
// class of bug where a command forgot to lazy-index and silently returned empty on a
// never-queried repo (only recall used to do it).
static void ensure_scope_indexed(store::Store &store, const Args &args) {
    if (!args.ref.empty()) return;  // ref = pure identity lookup, no path to scan
    std::string repo, idx;
    if (args.scope_set && !args.scope.empty()) {
        auto gid = gogol::resolve_git_identity(args.scope);
        if (gid && !gid->toplevel.empty()) { repo = gid->toplevel; idx = gid->common_dir; }
    } else if (!args.scope_set) {
        std::error_code ec;
        std::string cwd = fs::current_path(ec).string();
        if (!ec) {
            auto gid = gogol::resolve_git_identity(cwd);
            if (gid && !gid->toplevel.empty()) { repo = gid->toplevel; idx = gid->common_dir; }
        }
    }
    if (repo.empty()) return;  // not in a git repo => query whatever is already indexed
    // Probe first: if another process holds the write lock, don't block waiting to
    // acquire our own writable handle — stale-but-instant beats blocked. The read
    // query uses a READ-ONLY handle that doesn't contend under WAL.
    if (store.writer_busy()) return;
    // Resolve the model WITHOUT trusting args.model (only index/recall/remember run it
    // through resolve_model; the pure-graph reads — delve/links/literal — leave it empty).
    // If no model file is present, SKIP the refresh and serve what's already indexed:
    // a graph/metrics read must NOT hard-fail just because the embedder is unconfigured
    // (the embedder ctor std::exit(1)s on a bad path, so we must not even attempt it).
    std::string model = args.model;
    if (model.empty()) {
        if (const char *env = std::getenv("GOGOL_MODEL")) model = env;
        else { auto gc = load_global_config(); model = gc.model; }
        if (model.empty()) model = model_path();
    }
    std::error_code mec;
    if (model.empty() || !fs::exists(model, mec) || mec)
        return;  // no usable model => skip lazy-index, query what's indexed
    auto embedder = get_embedder_cfg(model);
    if (!embedder) return;  // no embedder => can't refresh; query what's indexed
    store::Store rw(cache_db_path_cli(), notes_db_path_cli());  // own writable handle
    ProgressSpinner spinner;
    auto progress = [&spinner](int cur, int tot, const std::string &f) {
        return spinner.update(cur, tot, f);
    };
    auto r = gogol::refresh_repo(rw, *embedder, repo, idx, gogol::Deadline{}, progress);
    if (r.ingested > 0) spinner.done(r.ingested);
    else spinner.clear();
}

// Resolve a SINGLE index for the symbol tools (delve/links/literal) from the shared
// scope resolver — --scope (name/path) + --ref (root SHA) + CWD default — so they pick
// a repo exactly like recall does (--scope name/path, --ref SHA, else CWD).
// Symbol lookups are single-repo by nature (a symbol lives in one index), so we take
// rsc.idx and ignore parent-dir union; an unresolved scope yields ResolvedIdx::none()
// = search all indexes (the historical --index-omitted default).
static gogol::ResolvedIdx resolve_cwd_idx(store::Store &store, const Args &args) {
    auto known = store.index_locations();
    std::error_code cwd_ec;
    gogol::RecallScopeInputs in;
    in.ref = args.ref;
    in.scope = args.scope;
    in.scope_set = args.scope_set;
    in.cwd = fs::current_path(cwd_ec).string();
    if (cwd_ec) in.cwd.clear();
    return gogol::resolve_recall_scope(in, known).idx;
}

// If --scope was given but is an invalid PATH (nonexistent, not a repo/dir), print the
// JSON error and return true so the caller bails — shared by every read verb so a bad
// --scope fails loudly (like `ls <nonexistent>`) instead of silently unioning the store.
static bool scope_rejected(store::Store &store, const Args &args) {
    if (!args.scope_set || args.scope.empty()) return false;
    std::error_code cwd_ec;
    gogol::RecallScopeInputs in;
    in.ref = args.ref;
    in.scope = args.scope;
    in.scope_set = args.scope_set;
    in.cwd = fs::current_path(cwd_ec).string();
    if (cwd_ec) in.cwd.clear();
    auto rsc = gogol::resolve_recall_scope(in, store.index_locations());
    if (!rsc.error.empty()) {
        std::cerr << "{\"error\":\"" + json_escape(rsc.error) + "\"}\n";
        return true;
    }
    return false;
}

// Shared JSON string escaper — all mem actions emit JSON (aligned with MCP) so
// output is machine-parseable / jq-friendly, one schema across CLI + MCP.
static std::string json_escape(const std::string &s){ std::string o; o.reserve(s.size()+8);
    for(char c: s){ if(c=='"'||c=='\\'){o.push_back('\\');o.push_back(c);}
        else if(c=='\n')o+="\\n"; else if(c=='\t')o+="\\t"; else if((unsigned char)c<0x20){} else o.push_back(c);} return o; }

// Output wrapper: consistent JSON envelope matching MCP format
// wraps results array and metadata (count, max_cosine, etc.)
struct JsonOutput {
    std::string results_json;  // already-stringified JSON array
    long count = 0;
    float max_cosine = 0.0f;
    bool low_confidence = false;
    
    std::string to_string() const {
        std::string out = "{\"results\":" + results_json;
        out += ",\"count\":" + std::to_string(count);
        
        char cs[16]; std::snprintf(cs, sizeof(cs), "%.4f", max_cosine);
        out += ",\"max_cosine\":" + std::string(cs);
        
        if (low_confidence) out += ",\"low_confidence\":true";
        out += "}";
        return out;
    }
};

// Index a PATH by its git identity — the configless, on-demand indexing entry.
// Resolves the git repo the path is in (gogol::resolve_git_identity), uses the git
// common_dir as the index identity (worktree-agnostic — all worktrees of a repo
// share the same common_dir so they share one index), and indexes the WHOLE
// toplevel. Extensions come from default_index_extensions (the built-in set).
//
// Identity model: idx = common_dir (full path), display tag = repo_tag(idx).
static int handle_index(store::Store &store, const Args &args) {
    // The path to index: --content carries it (reused; it's the only free-form path
    // arg on the mem subcommand), else the current directory.
    std::string dir = args.content;
    if (dir.empty()) {
        std::error_code ec;
        dir = fs::current_path(ec).string();
        if (ec) { std::cerr << "{\"error\":\"cannot determine current directory\"}\n"; return 1; }
    }
    std::error_code ec;
    if (!fs::exists(dir, ec)) { std::cerr << "{\"error\":\"path does not exist\",\"path\":\"" << json_escape(dir) << "\"}\n"; return 1; }

    auto id = gogol::resolve_git_identity(dir);
    if (!id || id->toplevel.empty()) {
        std::cerr << "{\"error\":\"not inside a git repository\",\"path\":\"" << json_escape(dir) << "\"}\n";
        return 1;
    }
    // Identity: use common_dir (the worktree-agnostic git identity) as idx.
    // Strip trailing .git - it's internal detail, not meaningful to users.
    std::string idx = gogol::strip_git_suffix(id->common_dir);
    if (idx.empty()) { std::cerr << "{\"error\":\"could not derive git identity\",\"toplevel\":\"" << json_escape(id->toplevel) << "\"}\n"; return 1; }

    auto embedder = get_embedder_cfg(args.model);
    if (!embedder) { std::cerr << "Error: could not load embedder\n"; return 1; }

    // Explicit index of a path = an UNBOUNDED refresh of its repo (no deadline): the
    // same shared entry the CLI query uses, just triggered directly.
    ProgressSpinner spinner;
    auto progress = [&spinner](int cur, int tot, const std::string &f) {
        return spinner.update(cur, tot, f);
    };
    IngestResult r = gogol::refresh_repo(store, *embedder, id->toplevel, idx, gogol::Deadline{}, progress);
    if (r.ingested > 0) spinner.done(r.ingested);
    else spinner.clear();
    if (!r.ok) { std::cerr << "{\"error\":\"index failed\",\"detail\":\"" << json_escape(r.err) << "\"}\n"; return 1; }
    auto s = store.stats();
    std::cout << "{\"index\":\"" << json_escape(repo_tag(idx)) << "\",\"idx\":\"" << json_escape(idx)
              << "\",\"toplevel\":\"" << json_escape(id->toplevel)
              << "\",\"ingested\":" << r.ingested << ",\"scanned\":" << r.read
              << ",\"purged\":" << r.purged << ",\"gc_purged\":" << r.gc_purged
              << ",\"entries\":" << s.entries << "}\n";
    return 0;
}

static int handle_get(store::Store &store, const Args &args) {
    // Fetch one entry's full content by id (the CLI twin of MCP `get`).
    if (args.uri.empty()) { std::cerr << "{\"error\":\"id required\"}\n"; return 1; }
    auto e = store.get_latest(args.uri);
    if (!e) { std::cerr << "{\"error\":\"no entry with id " << json_escape(args.uri) << "\"}\n"; return 1; }
    std::string idx; for (size_t i = 0; i < e->indexes.size(); i++) { if (i) idx += ","; idx += e->indexes[i]; }
    std::string o = "{";
    o += "\"id\":\"" + json_escape(e->id) + "\"";
    o += ",\"type\":\"" + json_escape(e->type) + "\"";
    o += ",\"source\":\"" + json_escape(e->source) + "\"";
    if (e->line > 0) o += ",\"line\":" + std::to_string(e->line);
    if (e->end_line > 0) o += ",\"end_line\":" + std::to_string(e->end_line);
    if (!e->supersedes.empty()) o += ",\"supersedes\":\"" + json_escape(e->supersedes) + "\"";
    if (!e->superseded_by.empty()) o += ",\"superseded_by\":\"" + json_escape(e->superseded_by) + "\"";
    if (!e->resolved_from.empty()) o += ",\"resolved_from\":\"" + json_escape(e->resolved_from) + "\"";
    o += ",\"indexes\":\"" + json_escape(idx) + "\"";
    o += ",\"content\":\"" + json_escape(e->content) + "\"";
    o += "}";
    std::cout << o << "\n";
    return 0;
}

static int handle_related(store::Store &store, const Args &args) {
    // Find entries related to a given entry by embedding similarity (the CLI twin
    // of MCP `related`). Returns code/docs ranked by semantic similarity to the
    // input entry — "what code does this doc describe?" or "what docs cover this code?"
    if (args.uri.empty()) { std::cerr << "{\"error\":\"related needs --id\"}\n"; return 1; }
    int k = args.top_k > 0 ? args.top_k : 10;
    auto hits = store.related(args.uri, k, args.type_filter);
    if (hits.empty()) {
        std::cerr << "{\"error\":\"no entry with id " << json_escape(args.uri) << " or no embedding\"}\n";
        return 1;
    }
    // ONE wrapped JSON object {"results":[...],"count":N} — same shape as recall/literal,
    // NOT newline-delimited objects (NDJSON). A consumer json.loads the whole stdout.
    std::string arr;
    int n = 0;
    for (const auto &hit : hits) {
        auto e = store.get(hit.id);
        if (!e) continue;
        if (!arr.empty()) arr += ",";
        arr += "{\"id\":\"" + json_escape(e->id) + "\"";
        arr += ",\"score\":" + std::to_string(hit.score);
        arr += ",\"type\":\"" + json_escape(e->type) + "\"";
        arr += ",\"source\":\"" + json_escape(e->source) + "\"";
        if (!e->symbol.empty()) arr += ",\"symbol\":\"" + json_escape(e->symbol) + "\"";
        if (e->line > 0) arr += ",\"line\":" + std::to_string(e->line);
        arr += "}";
        n++;
    }
    std::cout << "{\"results\":[" << arr << "],\"count\":" << n << "}\n";
    return 0;
}

static int handle_overlay(store::Store &store, const Args &args) {
    // Mark (or --stop: clear) a repo as an active overlay. Resolves the target path
    // (positional, default CWD) to its root SHA and sets the overlay flag on that
    // (ref, path) sighting. Recall then unions overlay-flagged repos' content.
    std::error_code ec;
    std::string path = !args.scope.empty() ? args.scope : fs::current_path(ec).string();
    auto gid = gogol::resolve_git_identity(path);
    if (!gid || gid->root_sha.empty()) {
        std::cerr << "{\"error\":\"not a git repo with a resolvable root commit: "
                  << json_escape(path) << "\"}\n";
        return 1;
    }
    bool on = !args.overlay_stop;
    if (!store.set_overlay(gid->root_sha, gid->toplevel, on)) {
        std::cerr << "{\"error\":\"could not set overlay\"}\n"; return 1;
    }
    std::cout << "{\"overlay\":" << (on ? "true" : "false")
              << ",\"root_sha\":\"" << json_escape(gid->root_sha) << "\""
              << ",\"path\":\"" << json_escape(gid->toplevel) << "\"}\n";
    return 0;
}

static int handle_literal(store::Store &store, const Args &args) {
    // Literal phrase search (exact-match, no embeddings). Returns all entries
    // containing the pattern, useful for known symbols or error messages.
    if (args.query_text.empty()) {
        std::cerr << "{\"error\":\"literal needs a pattern\"}\n";
        return 1;
    }
    if (scope_rejected(store, args)) return 1;
    int limit = args.top_k > 0 ? args.top_k : 50;
    auto hits = store.literal_search(args.query_text, limit, resolve_cwd_idx(store, args));
    if (hits.empty()) {
        std::cout << "{\"count\":0,\"results\":[]}\n";
        return 0;
    }
    std::string o = "{\"results\":[";
    bool first = true;
    for (const auto &h : hits) {
        if (!first) o += ",";
        o += "{\"id\":\"" + json_escape(h.id) + "\"";
        o += ",\"source\":\"" + json_escape(h.source) + "\"";
        o += ",\"line\":" + std::to_string(h.line);
        if (!h.symbol.empty()) o += ",\"symbol\":\"" + json_escape(h.symbol) + "\"";
        o += "}";
        first = false;
    }
    o += "],\"count\":" + std::to_string(hits.size()) + "}";
    std::cout << o << "\n";
    return 0;
}

static int handle_metrics(store::Store &store, const Args &args) {
    // Rank functions by cyclomatic complexity (the CLI twin of MCP `metrics`).
    // Optional path/file substring filter (positional [file]); --limit caps rows.
    //
    // SCOPE: reuses the SAME resolver as recall (gogol::resolve_recall_scope +
    // recall_restrict_refs), so metrics scopes identically — --index, else the CWD's
    // repo, else a parent-dir union of the child repos sighted under CWD. Crucially it
    // does NOT silently rank the WHOLE store when a scope was intended but unresolved
    // (e.g. invoked in a non-git parent with no indexed children): that returns an
    // empty, honest result instead of some unrelated repo's functions. Only an explicit
    // empty scope (no --index AND CWD resolves to nothing rankable) yields no rows.
    int limit = args.metrics_limit > 0 ? args.metrics_limit : 20;

    auto known = store.index_locations();
    std::error_code cwd_ec;
    gogol::RecallScopeInputs in;
    in.ref = args.ref;
    in.scope = args.scope;
    in.scope_set = args.scope_set;
    in.cwd = fs::current_path(cwd_ec).string();
    if (cwd_ec) in.cwd.clear();
    auto rsc = gogol::resolve_recall_scope(in, known);
    if (!rsc.error.empty()) { std::cerr << "{\"error\":\"" + json_escape(rsc.error) + "\"}\n"; return 1; }

    // Build the idx set to rank within: the parent-dir union (if CWD is a parent of
    // indexed repos) OR the single resolved idx (CWD is one repo / --scope / --ref).
    std::vector<std::string> scope_idxs =
        store.recall_restrict_refs(rsc.parent_dir, rsc.scoped);
    if (scope_idxs.empty() && !rsc.idx.value().empty())
        scope_idxs.push_back(rsc.idx.value());

    auto rows = store.top_complexity(limit, args.metrics_filter, scope_idxs);
    std::string arr;
    for (const auto &m : rows) {
        if (!arr.empty()) arr += ",";
        arr += "{\"file\":\"" + json_escape(m.file) + "\"";
        arr += ",\"name\":\"" + json_escape(m.name) + "\"";
        arr += ",\"line\":" + std::to_string(m.line);
        arr += ",\"complexity\":" + std::to_string(m.complexity);
        arr += ",\"lines\":" + std::to_string(m.lines);
        arr += ",\"params\":" + std::to_string(m.params);
        arr += ",\"returns\":" + std::to_string(m.returns);
        arr += ",\"max_depth\":" + std::to_string(m.max_depth) + "}";
    }
    std::cout << "{\"functions\":[" << arr << "],\"count\":" << rows.size() << "}\n";
    return 0;
}

static int handle_forget(store::Store &store, const Args &args) {
    if (args.uri.empty()) { std::cerr << "Error: --id required\n"; return 1; }
    int rc = store.forget_note(args.uri);
    store.commit();
    if (rc == 1) std::cout << "{\"forgotten\":\"" << json_escape(args.uri) << "\",\"ok\":true}\n";
    else if (rc == 0) { std::cerr << "No entry with id " << args.uri << "\n"; return 1; }
    else { std::cerr << "id " << args.uri << " is a disk-sourced entry, not a note — "
                        "delete the source file instead\n"; return 1; }
    return 0;
}

static int handle_remember(store::Store &store, const Args &args) {
    // Agent authors a proper note-document into memory. LLM is the source of
    // truth => no external source; the note lives in notes.db (content of record).
    // Path scope (path-prefix surfacing): explicit --scope wins; otherwise default
    // to the CWD's git toplevel (the repo the agent is working in) so a note
    // naturally attaches to that subtree; empty --scope ("") means a GLOBAL note.
    if (args.content.empty()) { std::cerr << "Error: --content required\n"; return 1; }
    auto embedder = get_embedder_cfg(args.model);
    if (!embedder) { std::cerr << "Error: could not load embedder\n"; return 1; }
    std::vector<float> v = embedder->embed_document(args.content);

    std::string scope;
    if (args.scope_set) {
        scope = args.scope;   // explicit (may be "" for a deliberately global note)
    } else {
        // Default: the git toplevel of the CWD (the subtree being worked in). Not in
        // a repo => global (empty). This keeps notes confined to where they're made.
        std::error_code ec;
        std::string cwd = fs::current_path(ec).string();
        if (!ec) {
            auto id = gogol::resolve_git_identity(cwd);
            if (id && !id->toplevel.empty()) scope = id->toplevel;
        }
    }
    if (args.consolidate_id.empty()) {
        std::string id = store.put_note(args.content, v, scope);
        store.commit();
        std::cout << "{\"id\":\"" << json_escape(id) << "\",\"type\":\"note\",\"scope\":\""
                  << json_escape(scope) << "\",\"notes\":" << store.count_by_type("note") << "}\n";
        return 0;
    }
    // --consolidate <id>: atomic forget(old) + put(new), one commit. The forget is
    // best-effort (not-found / code-doc id is reported, not fatal) so the new note
    // is never lost (R3).
    auto r = store.consolidate_note(args.consolidate_id, args.content, v, scope);
    store.commit();
    std::cout << "{\"id\":\"" << json_escape(r.id) << "\",\"type\":\"note\",\"scope\":\""
              << json_escape(scope) << "\",\"forgotten\":" << (r.forgotten ? "true" : "false")
              << ",\"refused\":" << (r.refused ? "true" : "false")
              << ",\"old_id\":\"" << json_escape(args.consolidate_id) << "\""
              << ",\"notes\":" << store.count_by_type("note") << "}\n";
    return 0;
}

static int handle_status(store::Store &store, const Args &args) {
    (void)store; (void)args;
    // Store stats live under the unified top-level `gogol status` now (daemon +
    // store in one place). Keep this default action as a pointer, not a duplicate.
    std::cerr << "Use `gogol status` for store stats (entries/edges/indexes + daemon).\n";
    return 0;
}

static int handle_delve(store::Store &store, const Args &args) {
    // Deep-dive a symbol (the CLI twin of MCP `delve`): its defining entry(ies) +
    // callers + callees (call graph) + imports (import graph of the defining file).
    // Composed entirely from existing store queries — no new graph code.
    std::string symbol = args.query_text;
    if (symbol.empty()) { std::cerr << "{\"error\":\"delve needs a symbol\"}\n"; return 1; }
    if (scope_rejected(store, args)) return 1;
    std::string tail = store::symbol_tail(symbol);

    // definitions: entries the symbol maps to (its own defining chunk).
    auto ids = store.ids_for_symbol(tail);
    if (tail != symbol) {
        auto raw = store.ids_for_symbol(symbol);
        ids.insert(ids.end(), raw.begin(), raw.end());
    }
    std::string def_source;
    std::string defs;
    for (const auto &id : ids) {
        auto e = store.get(id);
        if (!e) continue;
        if (!defs.empty()) defs += ",";
        defs += "{\"id\":\"" + json_escape(e->id) + "\",\"type\":\"" + json_escape(e->type)
              + "\",\"source\":\"" + json_escape(e->source) + "\"";
        if (e->line > 0) defs += ",\"line\":" + std::to_string(e->line);
        if (e->end_line > 0) defs += ",\"end_line\":" + std::to_string(e->end_line);
        defs += ",\"content\":\"" + json_escape(e->content) + "\"}";
        if (def_source.empty()) def_source = e->source;
    }

    auto edges_json = [](const std::vector<store::Store::Link> &rows) {
        std::string a;
        for (const auto &e : rows) {
            if (!a.empty()) a += ",";
            a += "{\"kind\":\"" + json_escape(e.kind) + "\",\"from\":\"" + json_escape(e.src)
               + "\",\"to\":\"" + json_escape(e.dst) + "\",\"index\":\"" + json_escape(e.idx) + "\"}";
        }
        return a;
    };
    gogol::ResolvedIdx ridx = resolve_cwd_idx(store, args);
    auto callers = store.links(tail, "call", "in", 1, ridx);
    auto callees = store.links(tail, "call", "out", 1, ridx);
    auto imports = def_source.empty() ? std::vector<store::Store::Link>{}
                                      : store.links(def_source, "import", "both", 1, ridx);

    std::cout << "{\"symbol\":\"" << json_escape(symbol) << "\",\"definitions\":[" << defs
              << "],\"callers\":[" << edges_json(callers)
              << "],\"callees\":[" << edges_json(callees)
              << "],\"imports\":[" << edges_json(imports) << "]}\n";
    return 0;
}

static int handle_links(store::Store &store, const Args &args) {
    std::string node = args.query_text;
    if (node.empty()) { std::cerr << "Error: --links needs --query <symbol-or-path>\n"; return 1; }
    if (scope_rejected(store, args)) return 1;
    std::string dir = args.link_dir.empty() ? "both" : args.link_dir;
    std::string tail = store::symbol_tail(node);
    gogol::ResolvedIdx ridx = resolve_cwd_idx(store, args);
    auto rows = store.links(tail, args.link_kind, dir, args.link_depth, ridx);
    if (tail != node) {
        auto raw = store.links(node, args.link_kind, dir, args.link_depth, ridx);
        rows.insert(rows.end(), raw.begin(), raw.end());
    }
    std::string arr;
    for (const auto &e : rows) {
        if (!arr.empty()) arr += ",";
        arr += "{\"kind\":\"" + json_escape(e.kind) + "\",\"from\":\"" + json_escape(e.src)
             + "\",\"to\":\"" + json_escape(e.dst) + "\",\"index\":\"" + json_escape(e.idx) + "\"}";
    }
    std::cout << "{\"node\":\"" << json_escape(node) << "\",\"direction\":\"" << json_escape(dir)
              << "\",\"links\":[" << arr << "],\"count\":" << rows.size() << "}\n";
    return 0;
}

static int handle_recall(store::Store &store, const Args &args) {
    auto embedder = get_embedder_cfg(args.model);
    if (!embedder) {
        std::cerr << "Error: could not load embedder\n";
        return 1;
    }
    // Lazy-index of the target repo happens centrally in cmd_store_impl via
    // ensure_scope_indexed(), before dispatch — so recall and every other read command
    // share the same HALT-UNTIL-INDEXED guarantee (no per-command drift).
    std::vector<float> q = embedder->embed_query(args.query_text);
    int k = args.top_k > 0 ? args.top_k : 5;
    int fetch = k;
    // Recall scope resolution (shared with MCP via gogol::resolve_recall_scope — the
    // ONE place ref/scope/index/CWD precedence + source_prefix + parent-dir rules live).
    auto known = store.index_locations();
    std::error_code cwd_ec;
    gogol::RecallScopeInputs in;
    in.ref = args.ref;
    in.scope = args.scope;
    in.scope_set = args.scope_set;
    in.cwd = fs::current_path(cwd_ec).string();  // CLI has a CWD (default-scope fallback)
    if (cwd_ec) in.cwd.clear();
    auto rsc = gogol::resolve_recall_scope(in, known);
    if (!rsc.error.empty()) { std::cerr << "{\"error\":\"" + json_escape(rsc.error) + "\"}\n"; return 1; }

    // Multi-ref restriction (parent-dir search + active overlays), stat-filtered to
    // live repos. recall_restrict_refs needs the store, so it stays at the call site.
    std::vector<std::string> restrict_refs =
        store.recall_restrict_refs(rsc.parent_dir, rsc.scoped);
    // A multi-ref scope spans MULTIPLE checkouts -> the single-checkout source_prefix
    // must not apply (it would exclude every unioned/overlaid repo).
    std::string qsource_prefix = rsc.source_prefix;
    if (!restrict_refs.empty()) qsource_prefix.clear();
    auto hits = store.query_hybrid(q, fetch, rsc.idx, args.type_filter, rsc.note_scope,
                                   args.query_text, qsource_prefix, restrict_refs);
    // Compute the best cosine across all non-edge hits for global confidence flag
    // (same logic as MCP recall handler).
    float global_max_cosine = 0.0f;
    for (const auto& h : hits) {
        if (!h.via_edge && h.max_cosine > global_max_cosine)
            global_max_cosine = h.max_cosine;
    }
    
    // Use unified formatter for consistent CLI + MCP output
    auto formatted = gogol::format_recall_results(hits, store, global_max_cosine);
    std::cout << formatted.to_string() << "\n";
    return 0;
}

static int cmd_store_impl(const Args &args, StoreAction action);

// Public entry: wrap the implementation so a transient "database is locked" (or any
// store error) surfaces as a clean JSON error on stderr instead of an uncaught
// exception -> process terminate with a scary ggml backtrace (llama.cpp installs a
// terminate handler). Exit 3 signals "busy/locked" so callers can retry.
int cmd_store(const Args &args, StoreAction action) {
    try {
        return cmd_store_impl(args, action);
    } catch (const std::exception &e) {
        std::string msg = e.what();
        bool locked = msg.find("locked") != std::string::npos || msg.find("busy") != std::string::npos;
        std::cerr << "{\"error\":\"" << (locked ? "database busy — another process holds the lock; retry shortly"
                                               : "mem operation failed")
                  << "\",\"detail\":\"" << msg.substr(0,180) << "\"}\n";
        return locked ? 3 : 1;
    }
}

static int cmd_store_impl(const Args &args, StoreAction action) {
    // Read-only actions (query/links/get/delve/metrics/literal/related/the stats
    // pointer) open SQLITE_OPEN_READONLY so they never contend with a concurrent
    // writer's lock. Mutating actions (add/forget/index) open rw.
    const bool read_only = (action == StoreAction::QUERY ||
                            action == StoreAction::LINKS ||
                            action == StoreAction::GET ||
                            action == StoreAction::DELVE ||
                            action == StoreAction::METRICS ||
                            action == StoreAction::LITERAL ||
                            action == StoreAction::RELATED ||
                            action == StoreAction::STATS);
    std::optional<store::Store> store_ro, store_rw;
    if (read_only) store_ro.emplace(cache_db_path_cli(), notes_db_path_cli(), store::Store::ReadOnly{});
    else           store_rw.emplace(cache_db_path_cli(), notes_db_path_cli());
    store::Store &store = read_only ? *store_ro : *store_rw;

    // ONE lazy-index chokepoint: every read command that can target the CWD repo gets
    // the same HALT-UNTIL-INDEXED guarantee here, before dispatch. Previously only
    // recall did this inline, so a first-ever delve/links/metrics/literal/related in a
    // never-queried repo silently returned empty. Centralising removes that per-command
    // drift (ref lookups + non-git dirs are no-ops inside the helper).
    if (action == StoreAction::QUERY   || action == StoreAction::LINKS ||
        action == StoreAction::DELVE   || action == StoreAction::METRICS ||
        action == StoreAction::LITERAL || action == StoreAction::RELATED) {
        ensure_scope_indexed(store, args);
    }

    switch (action) {
        case StoreAction::INDEX_PATH:  return handle_index(store, args);
        case StoreAction::FORGET:      return handle_forget(store, args);
        case StoreAction::ADD_NOTE:    return handle_remember(store, args);
        case StoreAction::STATS:       return handle_status(store, args);
        case StoreAction::LINKS:       return handle_links(store, args);
        case StoreAction::QUERY:       return handle_recall(store, args);
        case StoreAction::LITERAL:     return handle_literal(store, args);
        case StoreAction::GET:         return handle_get(store, args);
        case StoreAction::DELVE:       return handle_delve(store, args);
        case StoreAction::METRICS:     return handle_metrics(store, args);
        case StoreAction::RELATED:     return handle_related(store, args);
        case StoreAction::OVERLAY:     return handle_overlay(store, args);
    }
    return 1;
}
