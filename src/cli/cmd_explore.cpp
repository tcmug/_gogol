// cmd_explore.cpp — explore command: one-call deep dive into any symbol or topic
#include "cli/commands.h"
#include "adapters/file_adapter.h"
#include "config/config.h"
#include "config/utils.h"
#include "core/call_graph_query.h"
#include "core/format.h"
#include "core/operations.h"
#include "daemon/embed_client.h"
#include "daemon/rpc.h"
#include "embedding/embed_provider.h"
#include "storage/call_store.h"
#include "storage/import_store.h"
#include "storage/export_store.h"
#include "storage/type_store.h"
#include "storage/metrics_store.h"
#include "storage/docref_store.h"
#include "storage/index_file.h"
#include "storage/storage_backend.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using std::cerr; using std::map; using std::set;
using std::string; using std::vector;

// --- Code explore: function deep-dive ---

struct ExploreContext {
    string name;
    string index_name;
    string file;
    uint32_t line;
    uint32_t end_line;
    uint64_t hash;
    int show_lines;
    const map<string, IndexConfig>& configs;
    const vector<CallGraph>& graphs;
    const vector<string>& graph_names;
    string index_filter;
};

static void print_source(const ExploreContext& ctx, int func_lines, bool stale) {
    if (stale) {
        printf("(stale — file has changed since last index, re-index to update)\n\n");
        return;
    }
    if (ctx.line == 0 || !ctx.configs.count(ctx.index_name)) return;

    int lines_to_show = ctx.show_lines;
    if (lines_to_show == 0) {
        if (ctx.end_line > ctx.line)
            lines_to_show = ctx.end_line - ctx.line + 1;
        else if (func_lines > 0)
            lines_to_show = func_lines;
        else
            lines_to_show = 15;
    }
    string doc_path = ctx.file + ":" + std::to_string(ctx.line);
    auto result = op_get(EntryType::DOC, ctx.index_name, doc_path,
                         ctx.configs.at(ctx.index_name), lines_to_show);
    if (result.ok)
        printf("%s\n\n", result.message.c_str());
}

static void print_imports(const string& index_name, const string& file) {
    auto igraph = open_backend(index_name)->load_import_graph();
    auto file_imports = igraph.imports_of(file);
    vector<string> project_imports;
    for (auto& imp : file_imports) {
        if (imp.size() >= 2 && imp[0] == '.' && (imp[1] == '/' || imp[1] == '.'))
            project_imports.push_back(imp);
    }
    if (!project_imports.empty()) {
        printf("imports:\n  ");
        for (size_t i = 0; i < project_imports.size(); i++) {
            if (i > 0) printf(", ");
            if (i > 0 && i % 4 == 0) printf("\n  ");
            printf("%s", project_imports[i].c_str());
        }
        printf("\n\n");
    }
}

static void print_callers(const ExploreContext& ctx, CallGraphQuery& cg_query,
                          const map<string, MetricsStore>& all_metrics) {
    auto get_metrics_str = [&](const string& fname, const string& ffile, const string& idx) -> string {
        if (!all_metrics.count(idx)) return "";
        for (auto& m : all_metrics.at(idx).entries) {
            if (m.name == fname && m.file == ffile)
                return "  c:" + std::to_string(m.complexity) + " l:" + std::to_string(m.lines);
        }
        return "";
    };

    set<string> seen;
    vector<StoredCallEdge> callers;
    for (size_t i = 0; i < ctx.graphs.size(); i++) {
        for (auto& e : ctx.graphs[i].callers_of(ctx.name)) {
            string key = e.caller + "\t" + e.file;
            if (seen.count(key)) continue;
            seen.insert(key);
            callers.push_back(e);
        }
    }

    if (!callers.empty()) {
        printf("callers:\n");
        for (auto& c : callers) {
            string idx = cg_query.find_index_for(c.caller, c.file);
            if (idx.empty()) idx = ctx.index_name;
            string cloc = format_path(idx, c.file, c.line, ctx.configs);
            string metrics = get_metrics_str(c.caller, c.file, idx);
            printf("  %-30s %s%s\n", c.caller.c_str(), cloc.c_str(), metrics.c_str());
        }
        printf("\n");
    }
}

static void print_callees(const ExploreContext& ctx, CallGraphQuery& cg_query,
                          const map<string, MetricsStore>& all_metrics) {
    auto get_metrics_str = [&](const string& fname, const string& ffile, const string& idx) -> string {
        if (!all_metrics.count(idx)) return "";
        for (auto& m : all_metrics.at(idx).entries) {
            if (m.name == fname && m.file == ffile)
                return "  c:" + std::to_string(m.complexity) + " l:" + std::to_string(m.lines);
        }
        return "";
    };

    // Only show callees that are project-defined (have their own callers/callees)
    set<string> all_callers_set;
    for (auto& g : ctx.graphs)
        for (auto& e : g.edges)
            all_callers_set.insert(e.caller);

    set<string> seen;
    vector<StoredCallEdge> callees;
    for (size_t i = 0; i < ctx.graphs.size(); i++) {
        for (auto& e : ctx.graphs[i].callees_of(ctx.name)) {
            if (seen.count(e.callee)) continue;
            seen.insert(e.callee);
            if (all_callers_set.count(e.callee))
                callees.push_back(e);
        }
    }

    if (!callees.empty()) {
        printf("callees:\n");
        int shown = 0;
        for (auto& c : callees) {
            if (shown >= 10) {
                printf("  ... +%zu more\n", callees.size() - 10);
                break;
            }
            string idx = cg_query.find_index_for(c.callee, c.file);
            if (idx.empty()) idx = ctx.index_name;
            string cloc = format_path(idx, c.file, c.line, ctx.configs);
            string metrics = get_metrics_str(c.callee, c.file, idx);
            printf("  %-30s %s%s\n", c.callee.c_str(), cloc.c_str(), metrics.c_str());
            shown++;
        }
        printf("\n");
    }
}

static bool print_used_by(const string& name, const string& index_name, const string& file) {
    auto igraph = open_backend(index_name)->load_import_graph();

    // Try symbol-level first
    auto symbol_importers = igraph.files_importing_symbol(name);
    if (!symbol_importers.empty()) {
        printf("used by:\n");
        int shown = 0;
        for (auto& imp : symbol_importers) {
            if (shown >= 10) { printf("  ... +%zu more\n", symbol_importers.size() - 10); break; }
            printf("  %s:%s\n", index_name.c_str(), imp.c_str());
            shown++;
        }
        printf("\n");
        return true;
    }

    // Fall back to file-level importers
    auto importers = igraph.imported_by(file);
    auto last_slash = file.rfind('/');
    string basename = (last_slash != string::npos) ? file.substr(last_slash + 1) : file;
    auto dot = basename.rfind('.');
    string stem = (dot != string::npos) ? basename.substr(0, dot) : basename;
    if (importers.empty() && stem != file)
        importers = igraph.imported_by(stem);

    if (!importers.empty()) {
        printf("used by:\n");
        int shown = 0;
        for (auto& imp : importers) {
            if (shown >= 10) { printf("  ... +%zu more\n", importers.size() - 10); break; }
            printf("  %s:%s\n", index_name.c_str(), imp.c_str());
            shown++;
        }
        printf("\n");
        return true;
    }
    return false;
}

static void print_type_hierarchy(const string& name, const string& index_name) {
    auto tgraph = open_backend(index_name)->load_type_graph();
    auto parents = tgraph.parents_of(name);
    auto children = tgraph.children_of(name);
    if (!parents.empty() || !children.empty()) {
        printf("type hierarchy:\n");
        for (auto& p : parents)
            printf("  %s %s  %s:%s:%u\n", p.kind.c_str(), p.parent.c_str(),
                   index_name.c_str(), p.file.c_str(), p.line);
        if (!parents.empty() && !children.empty()) printf("  ---\n");
        for (auto& c : children)
            printf("  extended by %s  %s:%s:%u\n", c.name.c_str(),
                   index_name.c_str(), c.file.c_str(), c.line);
        printf("\n");
    }
}

static void print_related(const string& name, const string& file, uint32_t line,
                          const string& index_filter,
                          const map<string, IndexConfig>& configs) {
    if (!daemon_is_running()) return;
    RpcClient rpc;
    if (!rpc.connected()) return;

    QueryRequest req;
    req.query = name;
    req.index = index_filter;
    req.type_filter = -1;
    req.top_k = 5;
    vector<QueryResultEntry> rpc_results;
    string err = rpc.cmd_query(req, rpc_results);
    if (!err.empty() || rpc_results.empty()) return;

    printf("related:\n");
    for (auto& r : rpc_results) {
        if (r.path == file && r.line == line) continue;
        string rloc = format_path(r.index, r.path, r.line, configs);
        string heading = r.chunk.empty() ? "" : r.chunk;
        printf("  %-30s %s\n", heading.c_str(), rloc.c_str());
    }
}

// --- Doc references: markdown link graph sections ---
static void print_doc_refs(const string& file, const string& index_name,
                           const map<string, IndexConfig>& configs) {
    auto graph = open_backend(index_name)->load_docref_graph();

    auto edges = graph.references_of(file);
    vector<DocRefEdge> locals, externals;
    for (auto& e : edges) {
        if (e.kind == RefKind::Local) locals.push_back(e);
        else if (e.kind == RefKind::External) externals.push_back(e);
    }
    auto refby = graph.referenced_by(file);

    // 1. Local references
    if (!locals.empty()) {
        if (g_format == OutputFormat::AGENT) {
            printf("refs\n");
            for (auto& e : locals) {
                string ref = format_doc_ref(index_name, e.target, 1, e.text, configs);
                printf(">%s\n", ref.c_str());
            }
        } else {
            printf("references:\n");
            for (auto& e : locals)
                printf("  %-30s  \xE2\x86\x92 %s\n", e.text.c_str(), e.target.c_str());
        }
        printf("\n");
    }

    // 2. External references
    if (!externals.empty()) {
        if (g_format == OutputFormat::AGENT) {
            printf("ext\n");
            for (auto& e : externals)
                printf("^%s %s\n", e.target.c_str(), e.text.c_str());
        } else {
            printf("external references:\n");
            for (auto& e : externals)
                printf("  %-30s  \xE2\x86\x97 %s\n", e.text.c_str(), e.target.c_str());
        }
        printf("\n");
    }

    // 3. Referenced by (Local only)
    if (!refby.empty()) {
        if (g_format == OutputFormat::AGENT) {
            printf("refby\n");
            for (auto& src : refby) {
                string ref = format_doc_ref(index_name, src, 1, "", configs);
                printf("<%s\n", ref.c_str());
            }
        } else {
            printf("referenced by:\n");
            for (auto& src : refby)
                printf("  %s\n", src.c_str());
        }
        printf("\n");
    }
}

static int explore_code(const string& name, const string& index_name,
                        const string& file, uint32_t line, uint32_t end_line,
                        uint64_t hash, int show_lines,
                        const map<string, IndexConfig>& configs,
                        const vector<CallGraph>& graphs,
                        const vector<string>& graph_names,
                        const string& index_filter) {

    ExploreContext ctx{name, index_name, file, line, end_line, hash,
                       show_lines, configs, graphs, graph_names, index_filter};

    // Staleness check
    bool stale = false;
    if (hash != 0 && configs.count(index_name)) {
        FileAdapter fa(index_name, configs.at(index_name));
        auto status = fa.check_stale(file, hash);
        if (status == EntryStatus::STALE || status == EntryStatus::MISSING)
            stale = true;
    }

    // Function length from metrics
    int func_lines = 0;
    auto mstore = open_backend(index_name)->load_metrics();
    for (auto& m : mstore.entries)
        if (m.name == name && m.file == file) { func_lines = m.lines; break; }

    // Header
    string loc = format_path(index_name, file, line, configs);
    if (func_lines > 0)
        printf("%s  %s  (%d lines)\n\n", name.c_str(), loc.c_str(), func_lines);
    else
        printf("%s  %s\n\n", name.c_str(), loc.c_str());

    // Source
    print_source(ctx, func_lines, stale);

    // Doc references (markdown link graph) — only for .md/.mdx files
    {
        auto ends_with = [](const string& s, const string& suf) {
            return s.size() >= suf.size() &&
                   s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
        };
        if (ends_with(file, ".md") || ends_with(file, ".mdx")) {
            print_doc_refs(file, index_name, configs);
        }
    }

    // Imports
    print_imports(index_name, file);

    // Callers + callees
    map<string, MetricsStore> all_metrics;
    for (auto& gn : graph_names)
        all_metrics[gn] = open_backend(gn)->load_metrics();

    CallGraphQuery cg_query;
    cg_query.graphs = graphs;
    cg_query.graph_names = graph_names;

    print_callers(ctx, cg_query, all_metrics);
    print_callees(ctx, cg_query, all_metrics);

    // Used by (for types/fragments without call graph data)
    bool has_callers = false;
    for (size_t i = 0; i < graphs.size(); i++)
        if (!graphs[i].callers_of(name).empty()) { has_callers = true; break; }
    bool has_callees = false;
    for (size_t i = 0; i < graphs.size(); i++)
        if (!graphs[i].callees_of(name).empty()) { has_callees = true; break; }

    if (!has_callers && !has_callees)
        print_used_by(name, index_name, file);

    // Type hierarchy
    print_type_hierarchy(name, index_name);

    // Related (semantic)
    print_related(name, file, line, index_filter, configs);

    return 0;
}

// --- Knowledge/note explore: show content ---
static int explore_knowledge(const string& index_name, const string& path,
                             const map<string, IndexConfig>& configs) {
    if (configs.count(index_name)) {
        auto result = op_get(EntryType::NOTE, index_name, path, configs.at(index_name), 0);
        if (result.ok) {
            printf("%s\n\n%s\n",
                   format_loc(EntryType::NOTE, index_name, path).c_str(),
                   result.message.c_str());
            return 0;
        }
    }
    printf("%s\n\n(content not found)\n",
           format_loc(EntryType::NOTE, index_name, path).c_str());
    return 0;
}

int cmd_explore(const Args& args) {
    string name = args.query_text;
    if (name.empty()) {
        cerr << "Usage: gogol explore <name> [--index N] [-n lines]\n";
        return 1;
    }

    auto configs = load_config();
    vector<string> index_names;
    if (!args.name.empty()) {
        index_names = split_csv(args.name);
    } else {
        for (auto& [n, cfg] : configs)
            if (cfg.is_indexed()) index_names.push_back(n);
    }

    int show_lines = args.show_lines > 0 ? args.show_lines : 15;
    bool full_source = args.force; // --full flag
    if (full_source) show_lines = 0; // 0 = no limit in op_get

    // --- Load call graphs ---
    vector<CallGraph> graphs;
    vector<string> graph_names;
    for (auto& idx_name : index_names) {
        graphs.push_back(open_backend(idx_name)->load_call_graph());
        graph_names.push_back(idx_name);
    }

    // --- Try exact name lookup in index ---
    struct Definition {
        string index;
        string file;
        uint32_t line;
        uint32_t end_line;
        uint64_t hash;
    };
    vector<Definition> defs;

    for (auto& idx_name : index_names) {
        auto idx = open_backend(idx_name)->load_index();
        for (auto& e : idx.entries) {
            if (e.proto != EntryType::DOC) continue;
            bool exact = (e.chunk == name);
            bool sig = (e.chunk.size() > name.size() &&
                        e.chunk.substr(0, name.size()) == name &&
                        (e.chunk[name.size()] == '(' || e.chunk[name.size()] == ' '));
            if (exact || sig) {
                Definition d{idx_name, e.path, e.line, e.end_line, e.hash};
                // A signature-bearing heading ("main(int argc, ...)") is the real
                // declaration; a bare-name heading ("main") can be a spurious
                // chunk emitted mid-function (e.g. a macro expansion like
                // CLI11_PARSE that the grammar mis-parses). When both exist for
                // the SAME file, prefer the signature chunk's line.
                bool merged = false;
                for (auto& u : defs) {
                    if (u.index == d.index && u.file == d.file) {
                        if (sig) { u.line = e.line; u.end_line = e.end_line; u.hash = e.hash; }
                        merged = true;
                        break;
                    }
                }
                if (!merged) defs.push_back(d);
            }
        }
    }

    // Fallback: call graph + metrics store. The call graph knows which files
    // DEFINE a name, but its edge lines are CALL SITES (e.g. the first call
    // inside a function), not the declaration. The metrics store has the real
    // function span (line..end_line), so prefer it for the definition location
    // and fall back to the call-edge line only when metrics has no entry.
    if (defs.empty()) {
        for (size_t i = 0; i < graphs.size(); i++) {
            auto files = graphs[i].files_defining(name);
            auto mstore = open_backend(graph_names[i])->load_metrics();
            for (auto& f : files) {
                uint32_t line = 0, end_line = 0;
                bool from_metrics = false;
                for (auto& m : mstore.entries) {
                    if (m.name == name && m.file == f) {
                        line = m.line;           // true declaration line
                        end_line = m.line + (m.lines > 0 ? m.lines - 1 : 0);
                        from_metrics = true;
                        break;
                    }
                }
                if (!from_metrics) {
                    // No metrics entry — fall back to the first call-site line.
                    for (auto& e : graphs[i].edges) {
                        if (e.caller == name && e.file == f) {
                            line = e.line;
                            break;
                        }
                    }
                }
                defs.push_back({graph_names[i], f, line, end_line, 0});
            }
        }
    }

    // --- Found as code: full code explore ---
    if (!defs.empty()) {
        // Optional --file filter to disambiguate by path substring.
        if (!args.filter.empty()) {
            std::vector<Definition> filtered;
            for (auto& d : defs)
                if (d.file.find(args.filter) != std::string::npos) filtered.push_back(d);
            defs = std::move(filtered);
            if (defs.empty()) {
                cerr << "No definition of \"" << name << "\" in a file matching \""
                     << args.filter << "\"\n";
                return 1;
            }
        }

        // Dedup by (index, file, line) — the same def can surface via multiple
        // chunks or via both the index and the call-graph fallback.
        std::vector<Definition> uniq;
        for (auto& d : defs) {
            bool dup = false;
            for (auto& u : uniq)
                if (u.index == d.index && u.file == d.file && u.line == d.line) { dup = true; break; }
            if (!dup) uniq.push_back(d);
        }

        if (uniq.size() > 1) {
            // Ambiguous: multiple definitions. List them (round-trippable) and
            // let the caller narrow with --index or a more specific name,
            // rather than silently exploring an arbitrary one.
            cerr << uniq.size() << " definitions match \"" << name
                 << "\". Narrow with --file <path> (or --index):\n";
            for (auto& d : uniq) {
                std::string loc = format_doc_ref(d.index, d.file, d.line, "", configs);
                printf("%s\n", loc.c_str());
            }
            return 1;
        }

        auto& def = uniq[0];
        return explore_code(name, def.index, def.file, def.line, def.end_line,
                           def.hash, show_lines, configs, graphs, graph_names, args.name);
    }

    // --- Not found by name: fall back to semantic search ---
    if (!daemon_is_running()) {
        cerr << "No definition found for: " << name << "\n";
        cerr << "(Start daemon with 'gogol serve' for semantic search fallback)\n";
        return 1;
    }

    QueryResultEntry hit;
    {
        RpcClient rpc;
        if (!rpc.connected()) {
            cerr << "No definition found for: " << name << "\n";
            return 1;
        }

        QueryRequest req;
        req.query = name;
        req.index = args.name;
        req.type_filter = -1;
        req.top_k = 1;
        vector<QueryResultEntry> results;
        string err = rpc.cmd_query(req, results);
        if (!err.empty() || results.empty()) {
            cerr << "No results found for: " << name << "\n";
            return 1;
        }
        hit = results[0];
    } // RPC connection closed here

    // Route based on result type
    if (hit.proto == 1) {
        // MEM entry — show knowledge content
        return explore_knowledge(hit.index, hit.path, configs);
    }

    // FILE entry — code explore on the search result
    // Extract the function name from the chunk heading
    string func_name = hit.chunk;
    auto paren = func_name.find('(');
    if (paren != string::npos) func_name = func_name.substr(0, paren);
    auto space = func_name.find(' ');
    if (space != string::npos) func_name = func_name.substr(0, space);

    if (!func_name.empty()) {
        return explore_code(func_name, hit.index, hit.path, hit.line, 0,
                           0, show_lines, configs, graphs, graph_names, args.name);
    }

    // Generic file chunk — just show content
    string loc = format_path(hit.index, hit.path, hit.line, configs);
    printf("%s  %s\n\n", hit.chunk.c_str(), loc.c_str());
    if (hit.line > 0 && configs.count(hit.index)) {
        string doc_path = hit.path + ":" + std::to_string(hit.line);
        auto result = op_get(EntryType::DOC, hit.index, doc_path, configs[hit.index], show_lines);
        if (result.ok)
            printf("%s\n", result.message.c_str());
    }

    return 0;
}
