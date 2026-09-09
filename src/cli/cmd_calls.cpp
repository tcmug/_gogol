// cmd_calls.cpp — call graph, imports, metrics commands
#include "cli/commands.h"
#include "adapters/file_adapter.h"
#include "config/config.h"
#include "config/utils.h"
#include "core/call_graph_query.h"
#include "core/format.h"
#include "storage/call_store.h"
#include "storage/import_store.h"
#include "storage/metrics_store.h"
#include "storage/index_file.h"
#include "storage/storage_backend.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fnmatch.h>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using std::cerr; using std::map; using std::pair; using std::set; using std::string; using std::vector;

// --- cmd_calls ---

int cmd_calls(const Args& args) {
    string func_name = args.query_text;
    if (func_name.empty()) {
        cerr << "Usage: gogol calls <function_name> [--index N] [--depth D]\n";
        return 1;
    }

    vector<string> index_names;
    if (!args.name.empty()) {
        index_names = split_csv(args.name);
    } else {
        auto cfgs = load_config();
        for (auto& [n, cfg] : cfgs)
            if (cfg.is_indexed()) index_names.push_back(n);
    }

    CallGraphQuery cg;
    cg.load(index_names, args.verbose);

    int depth = args.depth > 0 ? args.depth : 1;

    // Recursive tree printer
    std::function<void(const string&, int, set<string>&, const string&, bool)> print_tree;
    print_tree = [&](const string& name, int level, set<string>& visited,
                     const string& prefix, bool is_caller) {
        auto edges = is_caller ? cg.find_callers(name) : cg.find_callees(name);
        for (size_t i = 0; i < edges.size(); i++) {
            auto& e = edges[i];
            if (visited.count(e.name)) continue;
            visited.insert(e.name);
            string loc = format_path(e.index, e.file, e.line, cg.configs);
            string stale = format_stale_suffix(cg.get_status(e.index, e.file));
            string verbose_info;
            if (args.verbose && cg.mstores.count(e.index)) {
                for (auto& m : cg.mstores[e.index].entries) {
                    if (m.name == e.name && m.file == e.file) {
                        verbose_info = " c:" + std::to_string(m.complexity) +
                                       " l:" + std::to_string(m.lines);
                        break;
                    }
                }
            }

            if (g_format == OutputFormat::AGENT) {
                for (int t = 1; t < level; t++) putchar('\t');
                printf("%c%s %s%s%s\n", is_caller ? '<' : '>',
                       e.name.c_str(), loc.c_str(), verbose_info.c_str(), stale.c_str());
                if (level < depth)
                    print_tree(e.name, level + 1, visited, "", is_caller);
            } else {
                bool is_last = true;
                for (size_t j = i + 1; j < edges.size(); j++)
                    if (!visited.count(edges[j].name)) { is_last = false; break; }
                string connector = is_last ? "\xe2\x94\x94 " : "\xe2\x94\x9c ";
                string child_prefix = prefix + (is_last ? "  " : "\xe2\x94\x82 ");
                printf("%s%s%-20s %s%s%s\n", prefix.c_str(), connector.c_str(),
                       e.name.c_str(), loc.c_str(), verbose_info.c_str(), stale.c_str());
                if (level < depth)
                    print_tree(e.name, level + 1, visited, child_prefix, is_caller);
            }
        }
    };

    auto callees = cg.find_callees(func_name);
    auto callers = cg.find_callers(func_name);

    // Disambiguate: many definitions → show per-file
    auto def_files = cg.defining_files(func_name);
    if (def_files.size() > 3 && depth == 1) {
        if (g_format == OutputFormat::AGENT)
            printf("!%zu definitions\n", def_files.size());
        else
            printf("Multiple definitions (%zu files). Showing per-file:\n\n", def_files.size());
        for (auto& file : def_files) {
            auto file_callees = cg.find_callees_in(func_name, file);
            if (file_callees.empty()) continue;
            if (g_format == OutputFormat::AGENT)
                printf("[%s]\n", fs::path(file).filename().string().c_str());
            else
                printf("  %s:\n", file.c_str());
            for (auto& e : file_callees) {
                string loc = format_path(e.index, e.file, e.line, cg.configs);
                if (g_format == OutputFormat::AGENT)
                    printf(">%s %s\n", e.name.c_str(), loc.c_str());
                else
                    printf("    \xe2\x94\x9c %s  %s\n", e.name.c_str(), loc.c_str());
            }
        }
        if (!callers.empty() && !args.calls_out) {
            if (g_format == OutputFormat::AGENT) printf("<%zu\n", callers.size());
            else printf("\n");
            set<string> visited; visited.insert(func_name);
            print_tree(func_name, 1, visited, "", true);
        }
        return 0;
    }

    if (callees.empty() && callers.empty()) {
        cerr << "No call edges found for: " << func_name << "\n";
        return 1;
    }

    bool show_callers = !args.calls_out && !callers.empty();
    bool show_callees = !args.calls_in && !callees.empty();

    if (show_callers) {
        if (g_format == OutputFormat::AGENT) printf("<%zu\n", callers.size());
        set<string> visited; visited.insert(func_name);
        print_tree(func_name, 1, visited, "", true);
    }

    if ((show_callers || show_callees) && g_format != OutputFormat::AGENT) {
        if (show_callers) printf("\n");
        string root_loc;
        for (size_t i = 0; i < cg.graphs.size(); i++) {
            auto files = cg.graphs[i].files_defining(func_name);
            if (!files.empty()) {
                auto cees = cg.graphs[i].callees_of(func_name);
                uint32_t line = cees.empty() ? 0 : cees[0].line;
                root_loc = format_path(cg.graph_names[i], files[0], line, cg.configs);
                break;
            }
        }
        if (root_loc.empty()) printf("%s\n", func_name.c_str());
        else printf("%s  %s\n", func_name.c_str(), root_loc.c_str());
        if (show_callees) printf("\n");
    }

    if (show_callees) {
        if (g_format == OutputFormat::AGENT) printf(">%zu\n", callees.size());
        set<string> visited; visited.insert(func_name);
        print_tree(func_name, 1, visited, "", false);
    }

    return 0;
}

// --- cmd_metrics ---

int cmd_metrics(const Args& args) {
    auto configs = load_config();
    vector<string> index_names;
    if (!args.name.empty())
        index_names = split_csv(args.name);
    else
        for (auto& [n, cfg] : configs)
            if (cfg.is_indexed()) index_names.push_back(n);

    vector<pair<string, FunctionMetrics>> all;
    for (auto& idx_name : index_names) {
        auto store = open_backend(idx_name)->load_metrics();
        if (!args.query_text.empty()) {
            for (auto& m : store.entries)
                if (m.file.find(args.query_text) != string::npos ||
                    m.name.find(args.query_text) != string::npos)
                    all.push_back({idx_name, m});
        } else {
            for (auto& m : store.entries) all.push_back({idx_name, m});
        }
    }

    if (all.empty()) {
        if (!args.query_text.empty())
            cerr << "No metrics found for: " << args.query_text << "\n";
        else
            cerr << "No metrics available. Run gogol index first.\n";
        return 1;
    }

    // Sort
    string sort_key = args.sort_by.empty() ? "c" : args.sort_by;
    if (sort_key == "c" || sort_key == "C")
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.complexity > b.second.complexity; });
    else if (sort_key == "l" || sort_key == "L")
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.lines > b.second.lines; });
    else if (sort_key == "p" || sort_key == "P")
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.params > b.second.params; });
    else if (sort_key == "r" || sort_key == "R")
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.returns > b.second.returns; });
    else if (sort_key == "d" || sort_key == "D")
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.max_depth > b.second.max_depth; });

    int limit = args.limit > 0 ? args.limit : (int)all.size();
    if (limit > (int)all.size()) limit = (int)all.size();

    if (g_format == OutputFormat::AGENT) {
        for (int i = 0; i < limit; i++) {
            auto& [idx, m] = all[i];
            string loc = format_path(idx, m.file, m.line, configs);
            // Append end line
            string end = std::to_string(m.line + m.lines - 1);
            printf("%s\t%s-%s\tc:%d\tl:%d\tp:%d\tr:%d\td:%d\n",
                   m.name.c_str(), loc.c_str(), end.c_str(),
                   m.complexity, m.lines, m.params, m.returns, m.max_depth);
        }
    } else {
        int w_name = 8, w_file = 4;
        for (int i = 0; i < limit; i++) {
            auto& [idx, m] = all[i];
            int nl = (int)m.name.size();
            string loc = format_path(idx, m.file, m.line, configs);
            char range[256];
            snprintf(range, sizeof(range), "%s-%d", loc.c_str(), m.line + m.lines - 1);
            int fl = (int)std::strlen(range);
            if (nl > w_name) w_name = nl;
            if (fl > w_file) w_file = fl;
        }
        struct winsize ws;
        int term_width = 120;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_width = ws.ws_col;
        int avail = term_width - 25;
        if (w_name + w_file + 4 > avail) {
            w_file = avail - w_name - 4;
            if (w_file < 20) { w_file = avail / 2; w_name = avail - w_file - 4; }
        }

        printf("%-*s  %-*s  %4s %5s %3s %3s %3s\n", w_name, "Function", w_file, "File", "C", "Lines", "P", "R", "D");
        for (int i = 0; i < limit; i++) {
            auto& [idx, m] = all[i];
            string loc = format_path(idx, m.file, m.line, configs);
            char range[256];
            snprintf(range, sizeof(range), "%s-%d", loc.c_str(), m.line + m.lines - 1);
            string name_str = m.name.size() > (size_t)w_name ? m.name.substr(0, w_name - 2) + ".." : m.name;
            string file_str = std::strlen(range) > (size_t)w_file ? string(range).substr(std::strlen(range) - w_file + 2) : string(range);
            if (file_str != string(range)) file_str = ".." + file_str.substr(2);
            printf("%-*s  %-*s  %4d %5d %3d %3d %3d\n",
                   w_name, name_str.c_str(), w_file, file_str.c_str(),
                   m.complexity, m.lines, m.params, m.returns, m.max_depth);
        }
    }
    return 0;
}

// --- cmd_affected ---

int cmd_affected(const Args& args) {
    // Collect input files
    vector<string> input_files = args.files;

    if (args.use_stdin) {
        string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            auto start = line.find_first_not_of(" \t\r\n");
            auto end = line.find_last_not_of(" \t\r\n");
            if (start != string::npos)
                input_files.push_back(line.substr(start, end - start + 1));
        }
    }

    if (input_files.empty()) {
        cerr << "Usage: gogol affected <files...> [--stdin] [--index N] [--depth D] [--filter glob]\n";
        cerr << "  Finds all files that transitively depend on the given files.\n";
        cerr << "  Example: git diff --name-only | gogol affected --stdin --filter '*.test.*'\n";
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

    int max_depth = args.depth > 0 ? args.depth : 5;

    // Load all import graphs
    map<string, ImportGraph> graphs;
    for (auto& idx_name : index_names)
        graphs[idx_name] = open_backend(idx_name)->load_import_graph();

    // Resolve input file paths to index-relative paths
    vector<pair<string, string>> seed_files; // (index, relative_path)

    for (auto& input : input_files) {
        // Check if prefixed with index: e.g. "backend:src/utils.ts"
        auto colon = input.find(':');
        if (colon != string::npos && colon > 0 && input[0] != '/' && input[0] != '.') {
            string idx = input.substr(0, colon);
            string rel = input.substr(colon + 1);
            if (graphs.count(idx)) {
                seed_files.push_back({idx, rel});
                continue;
            }
        }

        // Try to match against known files in each index. Match EXACTLY or by
        // path suffix ("/" + input) — never a bare substring, which would seed
        // from unrelated files that merely contain the name (e.g. an input of
        // "store-data-source.ts" must not match "…/other/store-data-source.ts.bak"
        // or unrelated paths that happen to contain the text).
        bool found = false;
        for (auto& [idx_name, graph] : graphs) {
            for (auto& [file, _] : graph.imports) {
                bool exact = (file == input);
                bool suffix = (input.find('/') == string::npos)
                    ? (file.size() > input.size() &&
                       file.compare(file.size() - input.size() - 1, input.size() + 1,
                                    "/" + input) == 0)
                    : (file.size() >= input.size() &&
                       file.compare(file.size() - input.size(), input.size(), input) == 0 &&
                       (file.size() == input.size() || file[file.size() - input.size() - 1] == '/'));
                if (exact || suffix) {
                    seed_files.push_back({idx_name, file});
                    found = true;
                    break;
                }
            }
        }

        // If not found as a file that imports something, seed it directly
        // so that imported_by can match it as a module path
        if (!found) {
            for (auto& [idx_name, graph] : graphs)
                seed_files.push_back({idx_name, input});
        }
    }

    // BFS: walk imported_by transitively to find all dependent files
    set<string> visited; // "index:file" dedup key
    set<string> affected; // results

    std::queue<std::tuple<string, string, int>> queue;
    for (auto& [idx, file] : seed_files) {
        string key = idx + ":" + file;
        if (visited.count(key)) continue;
        visited.insert(key);
        queue.push({idx, file, 0});
    }

    // Barrel/hub detection: a file imported by an outsized number of others is
    // almost always a re-export barrel (apis.ts, index.ts). Traversing THROUGH
    // it floods the result with everything downstream of the barrel, which is
    // not the real blast radius of the seed. We still REPORT such a hub as
    // affected, but we do not expand its (huge) importer set transitively.
    const size_t BARREL_FANIN = 40; // importers above this = treat as a hub

    while (!queue.empty()) {
        auto [idx_name, file, depth] = queue.front();
        queue.pop();

        if (depth >= max_depth) continue;
        if (!graphs.count(idx_name)) continue;

        auto& graph = graphs[idx_name];
        auto importers = graph.imported_by(file);

        // Do not propagate through a barrel/hub (but the hub itself was already
        // recorded as affected when it was enqueued by its own importer).
        if (depth > 0 && importers.size() > BARREL_FANIN) continue;

        for (auto& importer : importers) {
            string key = idx_name + ":" + importer;
            if (visited.count(key)) continue;
            visited.insert(key);
            affected.insert(key);
            queue.push({idx_name, importer, depth + 1});
        }
    }

    if (affected.empty()) return 0;

    // Output (apply --filter glob if provided)
    for (auto& entry : affected) {
        auto colon = entry.find(':');
        string idx_name = entry.substr(0, colon);
        string file = entry.substr(colon + 1);

        if (!args.filter.empty() && fnmatch(args.filter.c_str(), file.c_str(), 0) != 0)
            continue;

        if (g_path_mode == PathMode::ABS && configs.count(idx_name)) {
            auto& cfg = configs[idx_name];
            if (!cfg.paths.empty()) {
                fs::path abs = fs::path(cfg.paths[0]) / file;
                printf("%s\n", abs.c_str());
                continue;
            }
        }
        printf("%s:%s\n", idx_name.c_str(), file.c_str());
    }

    return 0;
}
