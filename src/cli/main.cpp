// main.cpp — CLI entry point and argument parsing
#include "cli/commands.h"
#include "config/config.h"
#include "core/format.h"
#include "daemon/embed_server.h"
#include "embedding/embed_provider.h"
#include "mcp/mcp_server.h"

#include <CLI11.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

static std::string resolve_model(const std::string& model) {
    if (!model.empty()) return model;
    if (const char* env = std::getenv("GOGOL_MODEL")) return env;
    auto gc = load_global_config();
    if (!gc.model.empty()) return gc.model;
    std::cerr << "Error: model path required. Set 'model' in ~/.gogol/config or GOGOL_MODEL env.\n";
    std::exit(1);
}

int main(int argc, char** argv) {
    g_format = detect_format();

    CLI::App app{"gogol \xe2\x80\x94 semantic search + knowledge store"};
    app.require_subcommand(1);

    std::string format_flag;
    app.add_option("--format", format_flag, "Output format: default, agent")->default_val("");

    Args args;

    auto* idx = app.add_subcommand("index", "Index files from config");
    idx->add_option("--index", args.name, "Index name");
    idx->add_flag("--force", args.force, "Force full re-index");

    auto* qry = app.add_subcommand("query", "Search");
    qry->add_option("text", args.query_text, "Search query")->required();
    qry->add_option("--index", args.name, "Index name (comma-separated)");
    qry->add_option("--type", args.type_arg, "Filter by entry type: doc, note, term");
    qry->add_option("-n", args.top_k, "Number of results")->default_val(5);
    qry->add_option("--show", args.show_lines, "Show N lines of content")->default_val(0)->expected(0, 1);
    qry->add_flag("--scores", args.scores, "Show RRF and cosine scores");
    qry->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    qry->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");
    qry->add_flag("-v,--verbose", args.verbose, "Show function metrics");

    auto* add = app.add_subcommand("add", "Store an entry");
    add->add_option("type", args.type_arg, "Entry type: doc, note, term")->required();
    add->add_option("index", args.name, "Index name")->required();
    add->add_option("path", args.path_arg, "File path (doc) / topic (note) / term")->required();
    add->add_option("content", args.content, "Content text");
    add->add_flag("--stdin", args.use_stdin, "Read content from stdin");
    add->add_option("-f,--file", args.content_file, "Read content from file");
    add->add_option("--sources", args.sources, "Source locations (comma-separated)");

    auto* rm = app.add_subcommand("rm", "Remove an entry");
    rm->add_option("type", args.type_arg, "Entry type: doc, note, term")->required();
    rm->add_option("index", args.name, "Index name")->required();
    rm->add_option("path", args.path_arg, "File path (doc) / topic (note) / term")->required();

    auto* get = app.add_subcommand("get", "Retrieve content");
    get->add_option("type", args.type_arg, "Entry type: doc, note, term")->required();
    get->add_option("index", args.name, "Index name")->required();
    get->add_option("path", args.path_arg, "File path (doc, may carry :line) / topic / term")->required();
    get->add_option("-n", args.get_lines, "Max lines to show (0 = full)")->default_val(0);

    auto* st = app.add_subcommand("status", "Show index stats");
    st->add_option("--index", args.name, "Index name");

    auto* lst = app.add_subcommand("list", "List entries");
    lst->add_option("type_or_index", args.type_arg, "Entry type (doc/note/term) OR index name if type omitted");
    lst->add_option("index", args.path_arg, "Index name (when a type token is given first)");
    lst->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    lst->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");

    app.add_subcommand("sync", "Git sync + w-mode export");
    app.add_subcommand("prune", "Remove indexes not in config");

    auto* calls = app.add_subcommand("calls", "Show call graph for a function");
    calls->add_option("name", args.query_text, "Function name")->required();
    calls->add_option("--index", args.name, "Index name (comma-separated)");
    calls->add_option("--depth", args.depth, "Traversal depth")->default_val(1);
    calls->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    calls->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");
    calls->add_flag("--in", args.calls_in, "Show only callers");
    calls->add_flag("--out", args.calls_out, "Show only callees");
    calls->add_flag("-v,--verbose", args.verbose, "Show function metrics");

    auto* metrics = app.add_subcommand("metrics", "Show function complexity metrics");
    metrics->add_option("file", args.query_text, "File path or function name (optional, shows all if omitted)");
    metrics->add_option("--index", args.name, "Index name (comma-separated)");
    metrics->add_option("--sort", args.sort_by, "Sort by: c(omplexity), l(ines), p(arams), r(eturns), d(epth)")->default_val("c");
    metrics->add_option("--limit,-n", args.limit, "Max results")->default_val(20);
    metrics->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    metrics->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");

    auto* affected = app.add_subcommand("affected", "Find files that transitively depend on given files");
    affected->add_option("files", args.files, "Changed file paths");
    affected->add_option("--index", args.name, "Index name (comma-separated)");
    affected->add_option("--depth,-d", args.depth, "Max dependency traversal depth")->default_val(5);
    affected->add_option("--filter,-f", args.filter, "Glob to filter results (e.g. '*.test.*', 'e2e/**')");
    affected->add_flag("--stdin", args.use_stdin, "Read file list from stdin");
    affected->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    affected->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");

    auto* explore = app.add_subcommand("explore", "Show definition, callers, callees, and related code for a symbol");
    explore->add_option("name", args.query_text, "Function/symbol name")->required();
    explore->add_option("--index", args.name, "Index name (comma-separated)");
    explore->add_option("--file", args.filter, "Disambiguate by file path substring");
    explore->add_option("-n", args.show_lines, "Lines of source to show")->default_val(15);
    explore->add_flag("--full", args.force, "Show complete function body");
    explore->add_option("--path", args.path_mode, "Path format: full, abs, short")->default_val("");
    explore->add_flag("--abs", [&](int64_t){ args.path_mode = "abs"; }, "Alias for --path abs");

    bool serve_stop = false, serve_status = false, serve_fg = false;
    std::string serve_tcp;
    auto* srv = app.add_subcommand("serve", "Embedding daemon");
    srv->add_flag("--stop", serve_stop, "Stop the daemon");
    srv->add_flag("--status", serve_status, "Show daemon status");
    srv->add_flag("--foreground", serve_fg, "Run in foreground");
    srv->add_option("--tcp", serve_tcp, "TCP listen address (e.g. 127.0.0.1:9400)");

    auto* mcp = app.add_subcommand("mcp", "Run an MCP server over stdio (JSON-RPC 2.0)");

    CLI11_PARSE(app, argc, argv);

    // Override format if flag provided
    if (format_flag == "agent" || format_flag == "compact") g_format = OutputFormat::AGENT;
    else if (format_flag == "default") g_format = OutputFormat::DEFAULT;

    // Resolve path mode
    if (args.path_mode == "abs") g_path_mode = PathMode::ABS;
    else if (args.path_mode == "short") g_path_mode = PathMode::SHORT;
    else if (args.path_mode == "full") g_path_mode = PathMode::FULL;

    if (qry->parsed() && qry->count("--show") && args.show_lines == 0)
        args.show_lines = 5;

    if (idx->parsed() || qry->parsed() || add->parsed())
        args.model = resolve_model(args.model);

    // Parse the entry-type token for add/rm/get (required positional).
    if (add->parsed() || rm->parsed() || get->parsed()) {
        if (!parse_entry_type(args.type_arg, args.entry_type)) {
            std::cerr << "Error: invalid entry type '" << args.type_arg
                      << "'. Expected: doc, note, or term.\n";
            return 1;
        }
    }

    // query --type is an optional filter.
    if (qry->parsed() && !args.type_arg.empty()) {
        if (!parse_entry_type(args.type_arg, args.entry_type)) {
            std::cerr << "Error: invalid --type '" << args.type_arg
                      << "'. Expected: doc, note, or term.\n";
            return 1;
        }
        args.has_type = true;
    }

    // list [type] <index>: first positional may be a type token or the index.
    if (lst->parsed()) {
        EntryType t;
        if (!args.path_arg.empty()) {
            // Two positionals given: first must be a type, second the index.
            if (!parse_entry_type(args.type_arg, args.entry_type)) {
                std::cerr << "Error: invalid entry type '" << args.type_arg
                          << "'. Expected: doc, note, or term.\n";
                return 1;
            }
            args.has_type = true;
            args.name = args.path_arg;
        } else if (parse_entry_type(args.type_arg, t)) {
            std::cerr << "Error: list <type> requires an index name.\n";
            return 1;
        } else {
            // Single positional: it's the index name, all types.
            args.name = args.type_arg;
            args.has_type = false;
        }
    }

    if (srv->parsed()) {
        if (serve_status) return server_status();
        if (serve_stop) return stop_embed_server() ? 0 : 1;
        args.model = resolve_model(args.model);
        if (serve_tcp.empty()) {
            auto gc = load_global_config();
            serve_tcp = gc.tcp;
        }
        return run_embed_server(args.model, serve_fg, serve_tcp);
    }

    if (idx->parsed()) return cmd_index(args);
    if (qry->parsed()) return cmd_query(args);
    if (add->parsed()) return cmd_add(args);
    if (rm->parsed()) return cmd_rm(args);
    if (get->parsed()) return cmd_get(args);
    if (app.got_subcommand("status")) return cmd_status(args);
    if (lst->parsed()) return cmd_list(args);
    if (app.got_subcommand("sync")) return cmd_sync(args);
    if (app.got_subcommand("prune")) return cmd_prune(args);
    if (calls->parsed()) return cmd_calls(args);
    if (metrics->parsed()) return cmd_metrics(args);
    if (affected->parsed()) return cmd_affected(args);
    if (explore->parsed()) return cmd_explore(args);

    if (mcp->parsed()) return cmd_mcp();

    return 1;
}
