// main.cpp — CLI entry point and argument parsing
//
// CLI11 instantiates std::wstring_convert (deprecated in C++17). clang reports
// the warning at the DECLARATION site inside the libc++ header (pulled in via
// <vector> from commands.h — before any pragma could wrap it), so the fix is
// libc++'s own switch, defined before the first libc++ include. Scoped to this
// TU: main.cpp is the only one that instantiates CLI11's converter, and our
// own deprecation warnings elsewhere (and on libstdc++) are unaffected.
#if defined(_LIBCPP_VERSION) || defined(__apple_build_version__)
#define _LIBCPP_DISABLE_DEPRECATION_WARNINGS 1
#endif
#include "cli/commands.h"
#include "config/config.h"
#include "embedding/embed_provider.h"
#include "mcp/mcp_server.h"
#include "mcp/stdio_shim.h"   // probe_daemon (non-spawning live-daemon query)
#include "store/store.h"

#include <filesystem>
#include <CLI11.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

static std::string resolve_model(const std::string& model) {
    if (!model.empty()) return model;
    if (const char* env = std::getenv("GOGOL_MODEL")) return env;
    auto gc = load_global_config();
    if (!gc.model.empty()) return gc.model;
    // Convention fallback: gogol_home()/model (a symlink to the active .gguf). This is
    // the documented zero-config path — use it when it exists before erroring.
    std::error_code ec;
    std::string mp = model_path();
    if (std::filesystem::exists(mp, ec) && !ec) return mp;
    std::cerr << "Error: model path required. Set 'model' in " << config_file_path()
              << ", GOGOL_MODEL env, or symlink " << mp << " to a .gguf.\n";
    std::exit(1);
}

// Minimal JSON string escaper for the status output (index paths may contain chars
// needing escaping; root_sha is hex). Local to main.cpp — cmd_store.cpp's copy is
// static and not visible here.
static std::string json_escape(const std::string &s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}

// Build the unified status JSON (store from cache.db + live bus via a NON-spawning daemon
// probe + connection label). Shared by one-shot `gogol status` and the --watch TUI so the
// two can't drift. See the status branch comments for the probe rationale.
static std::string build_status_json() {
    std::string db = cache_db_path();
    std::string notes = notes_db_path();
    std::string o = "{";
    try {
        store::Store store(db, notes, store::Store::ReadOnly{});
        auto s = store.stats();
        o += "\"store\":{\"path\":\"" + db + "\",\"notes_path\":\"" + notes
           + "\",\"entries\":" + std::to_string(s.entries)
           + ",\"vec_entries\":" + std::to_string(s.vec_entries)
           + ",\"edges\":" + std::to_string(s.edges)
           + ",\"indexes\":" + std::to_string(s.indexes)
           + ",\"notes\":" + std::to_string(store.count_by_type("note"));
        o += ",\"index_list\":[";
        bool first = true;
        for (const auto &info : store.index_breakdown()) {
            if (!first) o += ",";
            first = false;
            o += "{\"idx\":\"" + json_escape(info.idx) + "\"";
            if (!info.root_sha.empty())
                o += ",\"root_sha\":\"" + json_escape(info.root_sha) + "\"";
            o += ",\"entries\":" + std::to_string(info.entries);
            o += ",\"dirs\":[";
            for (size_t i = 0; i < info.dirs.size(); i++) {
                if (i) o += ",";
                o += "{\"dir\":\"" + json_escape(info.dirs[i].path) + "\"";
                o += ",\"overlay\":" + std::string(info.dirs[i].overlay ? "true" : "false") + "}";
            }
            o += "]}";
        }
        o += "]}";
    } catch (const std::exception &e) {
        (void)e;
        o += "\"store\":{\"error\":\"busy or unavailable\"}";
    }
    // Live daemon probe (NON-spawning): the message bus lives in the daemon's RAM, so a
    // store-only read can't see agents. If a daemon is up, lift its `bus` section in; else
    // report bus unavailable. Never spawn a daemon just to render status.
    {
        GlobalConfig gc = load_global_config();
        std::string sock = gc.socket_path.empty() ? (gogol_home() + "/daemon.sock")
                                                   : gc.socket_path;
        std::string bus_json = "{\"status\":\"no daemon running\"}";
        std::string connection = "none (store read directly from cache.db)";
        if (gc.socket_enabled) {
            auto reply = gogol::mcp::probe_daemon(
                sock,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"status\",\"arguments\":{}}}");
            if (reply) {
                gogol::Json env;
                if (gogol::json_parse(*reply, env, nullptr) && env.is_object()) {
                    auto res = env.find("result");
                    if (res != env.end()) {
                        auto content = res->find("content");
                        if (content != res->end() && content->is_array() && !content->empty()) {
                            auto txt = (*content)[0].find("text");
                            if (txt != (*content)[0].end() && txt->is_string()) {
                                gogol::Json inner;
                                if (gogol::json_parse(txt->get<std::string>(), inner, nullptr) &&
                                    inner.is_object()) {
                                    auto b = inner.find("bus");
                                    if (b != inner.end()) bus_json = b->dump();
                                }
                            }
                        }
                    }
                }
                connection = "unix socket: " + sock;
            }
        } else {
            connection = "none (socket disabled in [mcp])";
        }
        o += ",\"bus\":" + bus_json;
        o += ",\"connection\":\"" + json_escape(connection) + "\"";
    }
    o += "}";
    return o;
}

// Render one frame of the status TUI from a parsed status Json to stdout (ANSI).
static void render_status_frame(const gogol::Json& d) {
    auto num = [](const gogol::Json& j, const char* k) -> long {
        auto it = j.find(k); return (it != j.end() && it->is_number()) ? it->get<long>() : 0;
    };
    std::cout << "\033[2J\033[H";  // clear + home
    std::cout << "\033[1;36m  gogol status\033[0m\n";
    // connection line
    std::string conn = d.contains("connection") && d["connection"].is_string()
                         ? d["connection"].get<std::string>() : "?";
    std::cout << "  \033[2mconnection:\033[0m " << conn << "\n\n";
    // store cards
    if (d.contains("store") && d["store"].is_object()) {
        const auto& s = d["store"];
        std::cout << "  \033[1mStore\033[0m   entries " << num(s, "entries")
                  << "  ·  indexes " << num(s, "indexes")
                  << "  ·  edges " << num(s, "edges")
                  << "  ·  notes " << num(s, "notes") << "\n";
        auto il = s.find("index_list");
        if (il != s.end() && il->is_array()) {
            for (const auto& i : *il) {
                std::string idx = i.contains("idx") && i["idx"].is_string() ? i["idx"].get<std::string>() : "?";
                std::string label = idx;
                if (i.contains("dirs") && i["dirs"].is_array() && !i["dirs"].empty()) {
                    std::string dir = i["dirs"][0].value("dir", std::string());
                    auto slash = dir.find_last_of('/');
                    if (slash != std::string::npos) label = dir.substr(slash + 1);
                }
                std::cout << "    " << std::string(label).substr(0, 24);
                for (size_t p = label.size(); p < 24; p++) std::cout << ' ';
                std::cout << num(i, "entries") << " entries\n";
            }
        }
    }
    std::cout << "\n";
    // bus / agents
    if (d.contains("bus") && d["bus"].is_object()) {
        const auto& b = d["bus"];
        if (b.contains("agents_list")) {
            std::cout << "  \033[1mAgents\033[0m  " << num(b, "online") << " online / "
                      << num(b, "agents") << " seen  ·  " << num(b, "total_sent")
                      << " sent  ·  " << num(b, "queued") << " queued\n";
            for (const auto& a : b["agents_list"]) {
                std::string name = a.value("name", std::string("?"));
                bool online = a.value("online", false);
                std::cout << "    " << (online ? "\033[32m●\033[0m " : "\033[2m○\033[0m ")
                          << std::string(name).substr(0, 18);
                for (size_t p = name.size(); p < 18; p++) std::cout << ' ';
                std::cout << "sent " << num(a, "sent") << "  recv " << num(a, "received")
                          << "  queued " << num(a, "inbox_depth") << "\n";
            }
        } else {
            std::string st = b.value("status", std::string("unavailable"));
            std::cout << "  \033[1mAgents\033[0m  \033[2m" << st << "\033[0m\n";
        }
    }
    std::cout << "\n  \033[2mrefreshing every 2s · Ctrl-C to quit\033[0m\n";
    std::cout.flush();
}

static volatile std::sig_atomic_t g_tui_stop = 0;
static void tui_sigint(int) { g_tui_stop = 1; }

// The --watch TUI: redraw the status frame every 2s until Ctrl-C. Raw ANSI, no deps.
static int run_status_watch() {
    std::signal(SIGINT, tui_sigint);
    std::cout << "\033[?25l";  // hide cursor
    while (!g_tui_stop) {
        gogol::Json d;
        if (gogol::json_parse(build_status_json(), d, nullptr))
            render_status_frame(d);
        for (int i = 0; i < 20 && !g_tui_stop; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "\033[?25h\n";  // show cursor
    return 0;
}

int main(int argc, char** argv) {
    CLI::App app{"gogol — semantic search + knowledge store"};
    app.require_subcommand(1);

    Args args;

    auto* mcp = app.add_subcommand("mcp", "Run an MCP server over stdio (JSON-RPC 2.0)");
    GlobalConfig serve_defaults = load_global_config();
    std::string serve_host = serve_defaults.http_host.empty() ? "127.0.0.1" : serve_defaults.http_host;
    int serve_port = serve_defaults.http_port > 0 ? serve_defaults.http_port : 7777;
    auto* servec = app.add_subcommand("serve", "Run an HTTP MCP server + status page");
    servec->add_option("--host", serve_host, "Bind address (default 127.0.0.1; use 0.0.0.0 to expose — NO AUTH)");
    servec->add_option("--port", serve_port, "Port (default 7777)")->default_val(serve_port);

    // Configless on-demand indexing: `gogol index [dir]` — index a path by git
    // identity (index name = repo toplevel basename; nested repos + symlinks
    // skipped). Defaults to CWD.
    std::string index_dir;
    auto* indexc = app.add_subcommand("index", "Index a repo by git identity (configless). Defaults to CWD.");
    indexc->add_option("dir", index_dir, "Directory to index (any subdir of a git repo; defaults to CWD)");

    // Unified status: the memory store, as one JSON object.
    auto* statuscmd = app.add_subcommand("status", "Show unified status (memory store) as JSON");
    bool status_watch = false;
    statuscmd->add_flag("-w,--watch", status_watch, "Live TUI: redraw status every 2s (Ctrl-C to quit)");

    // --- Top-level verbs over the unified memory store (~/.gogol/cache.db + notes.db).
    // These replace the old `gogol mem --flag` grab-bag and mirror the MCP tools.

    // get <id> — fetch one entry's full content by id.
    auto* getc = app.add_subcommand("get", "Fetch a memory entry's full content by id");
    getc->add_option("id", args.uri, "Entry id (from a recall result)")->required();

    // recall <text> — semantic + call-graph hybrid search.
    auto* recallc = app.add_subcommand("recall", "Semantic search across the unified memory (hybrid)");
    recallc->add_option("text", args.query_text, "Natural-language / code query")->required();
    recallc->add_option("--type", args.type_filter, "Filter results by entry type: note | doc | code");
    recallc->add_option("--top", args.top_k, "Number of results")->default_val(5);
    auto* recall_scope = recallc->add_option("--scope", args.scope,
        "Query scope: a path, a repo name, or a directory holding several repos. "
        "Default: CWD's git toplevel; empty = global-only. For a portable, "
        "filesystem-independent identity use --ref instead.");
    recallc->add_option("--ref", args.ref,
        "Scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — "
        "a portable identity for when you have no shared filesystem (e.g. a container). "
        "Read-only lookup against an already-indexed repo; triggers no refresh. "
        "Discover a repo's ref via `gogol status`.");

    // Shared --scope/--ref help (the ONLY repo selectors now that --index is retired).
    const char *scope_help =
        "Scope to one repo: a path, a repo name (e.g. 'backend'), or a directory of "
        "repos. Default: the CWD's repo. For a portable SHA identity use --ref.";
    const char *ref_help =
        "Scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — "
        "a portable, filesystem-independent identity. Discover it via `gogol status`.";

    // literal <pattern> — exact phrase search (deterministic, no embeddings).
    auto* literalc = app.add_subcommand("literal", "Literal phrase search (exact match across indexed content)");
    literalc->add_option("pattern", args.query_text, "Exact phrase to search for")->required();
    auto* literal_scope = literalc->add_option("--scope", args.scope, scope_help);
    literalc->add_option("--ref", args.ref, ref_help);
    literalc->add_option("--limit", args.top_k, "Max results (default 50)")->default_val(50);

    // delve <symbol> — deep-dive a symbol: def + callers + callees + imports.
    auto* delvec = app.add_subcommand("delve", "Deep-dive a symbol: definition + callers + callees + imports");
    delvec->add_option("symbol", args.query_text, "Symbol name (e.g. processOrder)")->required();
    auto* delve_scope = delvec->add_option("--scope", args.scope, scope_help);
    delvec->add_option("--ref", args.ref, ref_help);

    // links <node> — graph navigation (call | import).
    auto* linksc = app.add_subcommand("links", "Graph navigation for a symbol or doc path (call|import)");
    linksc->add_option("node", args.query_text, "Symbol (e.g. processOrder) or doc path (e.g. docs/api.md)")->required();
    linksc->add_option("--kind", args.link_kind, "Link kind: call | import (default all)");
    linksc->add_option("--direction", args.link_dir, "Link direction: out | in | both (default both)");
    linksc->add_option("--depth", args.link_depth, "Transitive hops (default 1)");
    auto* links_scope = linksc->add_option("--scope", args.scope, scope_help);
    linksc->add_option("--ref", args.ref, ref_help);

    // metrics [file] — cyclomatic-complexity ranking.
    auto* metricsc = app.add_subcommand("metrics", "Rank functions by cyclomatic complexity");
    metricsc->add_option("file", args.metrics_filter, "Optional path/file substring filter");
    auto* metrics_scope = metricsc->add_option("--scope", args.scope, scope_help);
    metricsc->add_option("--ref", args.ref, ref_help);
    metricsc->add_option("--limit", args.metrics_limit, "Max functions (default 20)")->default_val(20);

    // remember <content> — author a note-document.
    auto* rememberc = app.add_subcommand("remember", "Author an LLM note-document into memory (type=note)");
    rememberc->add_option("content", args.content, "The note document (markdown, any length)")->required();
    auto* remember_scope = rememberc->add_option("--scope", args.scope,
        "Path this note is attached to (default: CWD's git toplevel; empty = global note)");
    rememberc->add_option("--consolidate", args.consolidate_id,
        "Note id to replace: archived in-place while the new note is written (atomic)");

    // forget <id> — remove a note by id.
    auto* forgetc = app.add_subcommand("forget", "Forget (remove) an LLM-authored note by id");
    forgetc->add_option("id", args.uri, "Note entry id")->required();

    // related <id> — find entries related by embedding similarity.
    auto* relatedc = app.add_subcommand("related", "Find entries related to a given entry by embedding similarity");
    relatedc->add_option("id", args.uri, "Entry id (from a recall result)")->required();
    relatedc->add_option("--type", args.type_filter, "Filter results by type: code | doc | note (default code+doc)");
    relatedc->add_option("--top", args.top_k, "Number of results")->default_val(10);

    auto* overlayc = app.add_subcommand("overlay",
        "Mark a repo as an active overlay (its content unions into recall from elsewhere). "
        "Default target: CWD's repo. Use --stop to clear.");
    overlayc->add_option("path", args.scope, "Repo path to overlay (default: CWD)");
    overlayc->add_flag("--stop", args.overlay_stop, "Clear the overlay flag instead of setting it");

    CLI11_PARSE(app, argc, argv);

    // Automatic daily backup of notes.db (Fibonacci retention, best-effort).
    // Runs once per invocation before any store opens; safe no-op if today's
    // backup already exists or notes.db doesn't yet. Never blocks on failure.
    maybe_backup_dbs();

    if (mcp->parsed()) return cmd_mcp();
    if (servec->parsed()) return cmd_serve(serve_host, serve_port);

    if (indexc->parsed()) {
        // `gogol index [dir]` — configless git-identity indexing. Reuses the
        // INDEX_PATH action (which reads the target dir from args.content).
        args.content = index_dir; // empty => cmd_store uses CWD
        args.model = resolve_model(args.model);
        return cmd_store(args, StoreAction::INDEX_PATH);
    }

    if (statuscmd->parsed()) {
        if (status_watch) return run_status_watch();
        std::cout << build_status_json() << "\n";
        return 0;
    }

    // get <id> — read-only fetch, no model needed.
    if (getc->parsed()) return cmd_store(args, StoreAction::GET);

    // recall <text> — semantic query; needs the embedder.
    if (recallc->parsed()) {
        args.scope_set = recall_scope->count() > 0;
        args.model = resolve_model(args.model);
        return cmd_store(args, StoreAction::QUERY);
    }

    // literal <pattern> — exact phrase search, no model needed.
    if (literalc->parsed()) {
        args.scope_set = literal_scope->count() > 0;
        return cmd_store(args, StoreAction::LITERAL);
    }

    // delve <symbol> — pure graph/store composition, no model needed.
    if (delvec->parsed()) {
        args.scope_set = delve_scope->count() > 0;
        return cmd_store(args, StoreAction::DELVE);
    }

    // links <node> — pure graph query, no model needed.
    if (linksc->parsed()) {
        args.scope_set = links_scope->count() > 0;
        return cmd_store(args, StoreAction::LINKS);
    }

    // metrics [file] — pure metrics query, no model needed.
    if (metricsc->parsed()) {
        args.scope_set = metrics_scope->count() > 0;
        return cmd_store(args, StoreAction::METRICS);
    }

    // remember <content> — author a note; needs the embedder.
    if (rememberc->parsed()) {
        args.scope_set = remember_scope->count() > 0;
        args.model = resolve_model(args.model);
        return cmd_store(args, StoreAction::ADD_NOTE);
    }

    // forget <id> — remove a note, no model needed.
    if (forgetc->parsed()) return cmd_store(args, StoreAction::FORGET);

    // related <id> — find related entries by embedding similarity, no model needed.
    if (relatedc->parsed()) return cmd_store(args, StoreAction::RELATED);
    if (overlayc->parsed()) return cmd_store(args, StoreAction::OVERLAY);

    return 1;
}
