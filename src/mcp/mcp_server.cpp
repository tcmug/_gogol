// gogol MCP server — stdio JSON-RPC 2.0 implementation.
//
// Protocol glue only. The Tool Registry (mcp/tool_registry.h) is the single
// source of truth for the tool catalog + param schema; execution reuses the
// existing daemon RpcClient (daemon/embed_client.h). No search/graph logic is
// reimplemented here.
//
// Wiring status (P3):
//   - query : WIRED  → RpcClient::cmd_query
//   - get   : WIRED  → RpcClient::cmd_get
//   - list  : WIRED  → RpcClient::cmd_list
//   - calls    : WIRED → CallGraphQuery (reads stores via open_backend; no daemon)
//   - affected : WIRED → import-graph BFS (open_backend; no daemon)
//   - explore  : WIRED → definition lookup + source + callers/callees + imports +
//                docrefs (open_backend); the semantic "related" section is added
//                only when the daemon is up (best-effort, never fatal).
#include "mcp/mcp_server.h"

#include <fnmatch.h>

#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "adapters/file_adapter.h"
#include "config/config.h"
#include "config/utils.h"
#include "core/call_graph_query.h"
#include "core/format.h"
#include "core/loc.h"
#include "core/operations.h"
#include "daemon/embed_client.h"
#include "daemon/rpc.h"
#include "mcp/json.h"
#include "mcp/tool_registry.h"
#include "storage/call_store.h"
#include "storage/docref_store.h"
#include "storage/import_store.h"
#include "storage/index_file.h"
#include "storage/metrics_store.h"
#include "storage/storage_backend.h"

namespace gogol {
namespace mcp {

namespace {

// JSON-RPC 2.0 standard error codes.
constexpr int kParseError = -32700;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

const char* kServerName = "gogol";
const char* kServerVersion = "0.1.0";
const char* kProtocolVersion = "2024-11-05";

// Server instructions (see handle_initialize). Front-loaded: tool selection +
// the core workflow rule sit in the first ~512 chars so they're available when
// the client decides how to use the server. Fully generic (no deployment
// specifics). Mirrors the SKILL decision-flow — keep the two aligned.
const char* kInstructions =
    "gogol is a semantic code-search + knowledge store over one or more indexed "
    "codebases. Choose a tool by intent: to understand a named function use "
    "`explore` (signature + source + callers + callees + imports in one call); "
    "to find code by concept/meaning use `query`; to trace call chains use "
    "`calls`; for the blast radius of a change use `affected`; to read a known "
    "location use `get`; to see what an index holds use `list`.\n\n"
    "Core workflow rule: after `query` returns a hit, `get` or `explore` that "
    "exact location \xE2\x80\x94 do NOT re-query with reworded terms (two queries "
    "max, then switch tools). Prefer `explore` over `query` whenever you already "
    "know the symbol name.\n\n"
    "Details: every entry is `<type> <index> <path>` \xE2\x80\x94 type is `doc` "
    "(a file chunk), `note` (a memory note), or `term` (a glossary term). "
    "Results lead with `type index path:line`, directly reusable as `get`/"
    "`explore` input. Pass `index` inline on each call to scope it (one field, "
    "no extra call); omit it to search all indexes. (Optional: for a long run "
    "against one index, `set_scope` once sets a default so you can omit `index` "
    "\xE2\x80\x94 skip it otherwise, inline `index` is simpler.) `affected` "
    "returning 0 usually means the file is an entry point that nothing imports "
    "(usually correct, not an error). `add_note`/`add_term` persist reusable "
    "findings.";

// Build a JSON-RPC success envelope: {jsonrpc, id, result}.
Json make_response(const Json& id, Json result) {
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = std::move(result);
    return r;
}

// Build a JSON-RPC error envelope: {jsonrpc, id, error:{code,message}}.
Json make_error(const Json& id, int code, const std::string& message) {
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = std::move(err);
    return r;
}

// Wrap a ToolResult as the MCP tools/call result:
//   { content: [ { type:"text", text:<serialized json> } ], isError }
Json wrap_tool_result(const ToolResult& tr) {
    Json block = Json::object();
    block["type"] = "text";
    if (tr.is_error) {
        // Serialize the error as a JSON object so the agent gets structured text.
        Json e = Json::object();
        e["error"] = tr.error;
        block["text"] = e.dump();
    } else {
        block["text"] = tr.content.dump();
    }
    Json content = Json::array();
    content.push_back(std::move(block));

    Json result = Json::object();
    result["content"] = std::move(content);
    result["isError"] = tr.is_error;
    return result;
}

// --- Handlers: reuse the existing daemon RpcClient; never reimplement search/
// graph logic. Bound to tool names ONCE in tool_dispatch() below — the single
// dispatch site. The registry (tool_registry.cpp) owns the catalog + schema;
// this file owns execution (it legitimately depends on RpcClient, which the
// daemon-free registry TU must not).

ToolResult handle_query(const ToolArgs& a, Session& s) {
    RpcClient rpc;
    if (!rpc.connected())
        return ToolResult::fail("No gogol daemon running. Start it with: gogol serve");
    QueryRequest req;
    req.query = a.get_str("text");
    req.index = a.has("index") ? a.get_str("index") : s.default_index;
    req.type_filter = -1;
    std::string type_tok = a.has("type") ? a.get_str("type") : s.default_type;
    if (!type_tok.empty()) { EntryType et; if (parse_entry_type(type_tok, et)) req.type_filter = (int)et; }
    req.top_k = a.has("limit") ? (int)a.get_int("limit") : 5;
    if (req.top_k <= 0) req.top_k = 5;
    req.show_lines = 0; req.scores = true;
    std::vector<QueryResultEntry> results;
    std::string err = rpc.cmd_query(req, results);
    if (!err.empty()) return ToolResult::fail(err);
    Json arr = Json::array();
    for (const auto& r : results) {
        Json e = Json::object();
        e["index"]=r.index; e["path"]=r.path; e["line"]=r.line; e["chunk"]=r.chunk;
        e["proto"]=r.proto; e["type"]=(r.proto==1?"note":r.proto==2?"term":"doc");
        e["status"]=r.status; e["score"]=r.score; e["cosine"]=r.cosine;
        if (!r.snippet.empty()) e["snippet"]=r.snippet;
        arr.push_back(std::move(e));
    }
    Json out = Json::object(); out["results"]=std::move(arr); out["count"]=results.size();
    // Cache the hit list so a follow-up `get result N` (or explore) can resolve
    // a cursor without re-querying (P4 result cursors).
    s.set_last_results(out["results"]);
    return ToolResult::ok(std::move(out));
}

ToolResult handle_get(const ToolArgs& a, Session& s) {
    RpcClient rpc;
    if (!rpc.connected())
        return ToolResult::fail("No gogol daemon running. Start it with: gogol serve");

    // Optional result-cursor shortcut: `result: N` resolves the type/index/path
    // (and line) from the last query/calls hit list, so the agent can do
    // query → get {result:0} without re-typing the location. Explicit
    // type/index/path args still win over the cursor.
    std::string type_tok = a.get_str("type");
    std::string index = a.has("index") ? a.get_str("index") : s.default_index;
    std::string path = a.get_str("path");
    if (a.has("result")) {
        ResultRef ref = s.result_ref(a.get_int("result"));
        if (!ref.valid)
            return ToolResult::fail("no cached result at index " +
                                    std::to_string(a.get_int("result")) +
                                    " — run query/calls first");
        if (type_tok.empty()) type_tok = ref.type.empty() ? "doc" : ref.type;
        if (index.empty()) index = ref.index;
        if (path.empty()) {
            path = ref.path;
            if (ref.line > 0) path += ":" + std::to_string(ref.line);
        }
    }

    if (type_tok.empty())
        return ToolResult::fail("missing required parameter: type (or a `result` cursor)");
    if (index.empty())
        return ToolResult::fail("missing required parameter: index (or a `result` cursor)");
    if (path.empty())
        return ToolResult::fail("missing required parameter: path (or a `result` cursor)");

    EntryType et;
    if (!parse_entry_type(type_tok, et))
        return ToolResult::fail("invalid type: expected doc | note | term");
    EntryRef ref; ref.type=et; ref.index=index; ref.path=path;
    int max_lines = a.has("lines") ? (int)a.get_int("lines") : 0;
    std::string content = rpc.cmd_get(ref, max_lines);
    if (content.empty())
        return ToolResult::fail("not found or empty: " + type_tok + " " + ref.index + " " + ref.path);
    Json out = Json::object();
    out["type"]=type_tok; out["index"]=ref.index; out["path"]=ref.path; out["content"]=content;
    return ToolResult::ok(std::move(out));
}

ToolResult handle_list(const ToolArgs& a, Session& s) {
    RpcClient rpc;
    if (!rpc.connected())
        return ToolResult::fail("No gogol daemon running. Start it with: gogol serve");
    std::string index = a.has("index") ? a.get_str("index") : s.default_index;

    // No index specified → return an OVERVIEW of available indexes (name +
    // entry counts), NOT an empty list. The daemon's LIST needs a specific
    // index; without this branch, `list {}` returned {count:0} and misled the
    // agent into thinking gogol was empty. Mirrors the CLI, which refuses
    // list-without-index and points at `status`.
    if (index.empty()) {
        Json indexes = Json::array();
        size_t total = 0;
        for (auto& [name, cfg] : load_config()) {
            if (!cfg.is_indexed()) continue;
            IndexCounts c = open_backend(name)->load_index_counts();
            Json ji = Json::object();
            ji["index"] = name;
            ji["docs"] = c.file_count;
            ji["notes"] = c.mem_count;
            indexes.push_back(std::move(ji));
            total += c.file_count + c.mem_count;
        }
        Json out = Json::object();
        size_t n_indexes = indexes.size();
        out["indexes"] = std::move(indexes);
        out["index_count"] = n_indexes;
        out["total_entries"] = total;
        out["hint"] = "No index specified — this is an overview. Call list with "
                      "an `index` (or query/explore with one) to work inside a "
                      "specific index.";
        return ToolResult::ok(std::move(out));
    }

    bool has_type = false; EntryType et = EntryType::DOC;
    std::string type_tok = a.has("type") ? a.get_str("type") : s.default_type;
    if (!type_tok.empty()) {
        if (!parse_entry_type(type_tok, et)) return ToolResult::fail("invalid type: expected doc | note | term");
        has_type = true;
    }
    std::vector<ListResultEntry> entries;
    std::string err = rpc.cmd_list(et, has_type, index, entries);
    if (!err.empty()) return ToolResult::fail(err);
    Json arr = Json::array();
    for (const auto& e : entries) {
        Json j = Json::object();
        j["proto"]=e.proto; j["type"]=(e.proto==1?"note":e.proto==2?"term":"doc");
        j["index"]=e.index; j["path"]=e.path; j["line"]=e.line;
        if (!e.chunk.empty()) j["chunk"]=e.chunk;
        arr.push_back(std::move(j));
    }
    Json out = Json::object(); out["entries"]=std::move(arr); out["count"]=entries.size();
    return ToolResult::ok(std::move(out));
}

// --- calls: reuse CallGraphQuery (find_callers/find_callees). Reads stores via
// open_backend, so it works with NO daemon. Mirrors cmd_calls's recursive
// print_tree walk, but emits JSON edge lists instead of text.
Json edge_to_json(const Edge& e) {
    Json j = Json::object();
    j["name"] = e.name;
    j["file"] = e.file;
    j["index"] = e.index;
    j["line"] = e.line;
    return j;
}

// Collect edges recursively (depth>1), same visited-set + walk shape as
// cmd_calls's print_tree, producing a flat JSON array.
void collect_edges_recursive(CallGraphQuery& cg, const std::string& name, int level,
                             int depth, bool is_caller, std::set<std::string>& visited,
                             Json& out) {
    auto edges = is_caller ? cg.find_callers(name) : cg.find_callees(name);
    for (const auto& e : edges) {
        if (visited.count(e.name)) continue;
        visited.insert(e.name);
        Json j = edge_to_json(e);
        j["depth"] = level;
        out.push_back(std::move(j));
        if (level < depth)
            collect_edges_recursive(cg, e.name, level + 1, depth, is_caller, visited, out);
    }
}

ToolResult handle_calls(const ToolArgs& a, Session& s) {
    std::string name = a.get_str("name");
    if (name.empty()) return ToolResult::fail("missing required parameter: name");

    std::vector<std::string> index_names;
    std::string idx = a.has("index") ? a.get_str("index") : s.default_index;
    if (!idx.empty()) {
        index_names = split_csv(idx);
    } else {
        for (auto& [n, cfg] : load_config())
            if (cfg.is_indexed()) index_names.push_back(n);
    }

    CallGraphQuery cg;
    cg.load(index_names, /*verbose=*/false);

    int depth = a.has("depth") ? (int)a.get_int("depth") : 1;
    if (depth < 1) depth = 1;

    std::string direction = a.get_str("direction");  // "in" | "out" | ""
    bool want_callers = (direction != "out");
    bool want_callees = (direction != "in");

    Json callers = Json::array();
    Json callees = Json::array();

    if (want_callers) {
        if (depth > 1) {
            std::set<std::string> visited; visited.insert(name);
            collect_edges_recursive(cg, name, 1, depth, /*is_caller=*/true, visited, callers);
        } else {
            for (const auto& e : cg.find_callers(name)) callers.push_back(edge_to_json(e));
        }
    }
    if (want_callees) {
        if (depth > 1) {
            std::set<std::string> visited; visited.insert(name);
            collect_edges_recursive(cg, name, 1, depth, /*is_caller=*/false, visited, callees);
        } else {
            for (const auto& e : cg.find_callees(name)) callees.push_back(edge_to_json(e));
        }
    }

    Json out = Json::object();
    out["name"] = name;
    out["callers"] = std::move(callers);
    out["callees"] = std::move(callees);
    out["count_callers"] = out["callers"].size();
    out["count_callees"] = out["callees"].size();

    // Cache a flat, get-compatible cursor list (callers then callees). Each edge
    // becomes {type:"doc", index, path:<file>, line, name} so a follow-up
    // `get result N` resolves an edge's location without re-querying.
    Json cursors = Json::array();
    auto push_edges = [&](const Json& edges) {
        if (!edges.is_array()) return;
        for (const auto& e : edges) {
            if (!e.is_object()) continue;
            Json c = Json::object();
            c["type"] = "doc";
            auto get_s = [&](const char* k) {
                auto it = e.find(k);
                return (it != e.end() && it->is_string()) ? it->get<std::string>() : std::string();
            };
            c["index"] = get_s("index");
            c["path"] = get_s("file");
            c["name"] = get_s("name");
            auto lit = e.find("line");
            if (lit != e.end() && lit->is_number_integer()) c["line"] = lit->get<long>();
            cursors.push_back(std::move(c));
        }
    };
    push_edges(out["callers"]);
    push_edges(out["callees"]);
    s.set_last_results(cursors);

    return ToolResult::ok(std::move(out));
}

// --- affected: reuse cmd_affected's import-graph BFS (open_backend; no daemon).
// Same FIXED seed-matching (exact / path-suffix, never bare substring) and the
// barrel-fan-in guard, producing a JSON list of "index:path" dependents.
ToolResult handle_affected(const ToolArgs& a, Session& s) {
    // files: required array of strings.
    std::vector<std::string> input_files;
    if (a.obj.is_object()) {
        auto it = a.obj.find("files");
        if (it != a.obj.end() && it->is_array()) {
            for (const auto& v : *it)
                if (v.is_string()) input_files.push_back(v.get<std::string>());
        } else if (it != a.obj.end() && it->is_string()) {
            input_files.push_back(it->get<std::string>());
        }
    }
    if (input_files.empty())
        return ToolResult::fail("missing required parameter: files (array of file paths)");

    auto configs = load_config();
    std::vector<std::string> index_names;
    std::string idx = a.has("index") ? a.get_str("index") : s.default_index;
    if (!idx.empty()) {
        index_names = split_csv(idx);
    } else {
        for (auto& [n, cfg] : configs)
            if (cfg.is_indexed()) index_names.push_back(n);
    }

    int max_depth = a.has("depth") ? (int)a.get_int("depth") : 0;
    if (max_depth <= 0) max_depth = 5;
    std::string filter = a.get_str("filter");

    std::map<std::string, ImportGraph> graphs;
    for (auto& idx_name : index_names)
        graphs[idx_name] = open_backend(idx_name)->load_import_graph();

    // Resolve seeds (index, relative_path) — mirrors cmd_affected exactly.
    std::vector<std::pair<std::string, std::string>> seed_files;
    for (auto& input : input_files) {
        auto colon = input.find(':');
        if (colon != std::string::npos && colon > 0 && input[0] != '/' && input[0] != '.') {
            std::string ix = input.substr(0, colon);
            std::string rel = input.substr(colon + 1);
            if (graphs.count(ix)) { seed_files.push_back({ix, rel}); continue; }
        }
        bool found = false;
        for (auto& [idx_name, graph] : graphs) {
            for (auto& [file, _] : graph.imports) {
                bool exact = (file == input);
                bool suffix = (input.find('/') == std::string::npos)
                    ? (file.size() > input.size() &&
                       file.compare(file.size() - input.size() - 1, input.size() + 1,
                                    "/" + input) == 0)
                    : (file.size() >= input.size() &&
                       file.compare(file.size() - input.size(), input.size(), input) == 0 &&
                       (file.size() == input.size() || file[file.size() - input.size() - 1] == '/'));
                if (exact || suffix) { seed_files.push_back({idx_name, file}); found = true; break; }
            }
        }
        if (!found)
            for (auto& [idx_name, graph] : graphs)
                seed_files.push_back({idx_name, input});
    }

    std::set<std::string> visited, affected;
    std::queue<std::tuple<std::string, std::string, int>> queue;
    for (auto& [ix, file] : seed_files) {
        std::string key = ix + ":" + file;
        if (visited.count(key)) continue;
        visited.insert(key);
        queue.push({ix, file, 0});
    }

    const size_t BARREL_FANIN = 40;
    while (!queue.empty()) {
        auto [idx_name, file, depth] = queue.front();
        queue.pop();
        if (depth >= max_depth) continue;
        if (!graphs.count(idx_name)) continue;
        auto& graph = graphs[idx_name];
        auto importers = graph.imported_by(file);
        if (depth > 0 && importers.size() > BARREL_FANIN) continue;
        for (auto& importer : importers) {
            std::string key = idx_name + ":" + importer;
            if (visited.count(key)) continue;
            visited.insert(key);
            affected.insert(key);
            queue.push({idx_name, importer, depth + 1});
        }
    }

    Json arr = Json::array();
    for (auto& entry : affected) {
        auto colon = entry.find(':');
        std::string file = entry.substr(colon + 1);
        if (!filter.empty() && fnmatch(filter.c_str(), file.c_str(), 0) != 0) continue;
        arr.push_back(entry);
    }

    Json out = Json::object();
    out["affected"] = std::move(arr);
    out["count"] = out["affected"].size();
    return ToolResult::ok(std::move(out));
}

// --- explore: reuse cmd_explore's core (open_backend index lookup + call graph
// + metrics fallback) to BUILD a JSON deep-dive rather than printing. Sections:
// definition (index/file/line/end_line/func_lines), source snippet, callers,
// callees (project-only), imports, docrefs, and best-effort related (daemon).
ToolResult handle_explore(const ToolArgs& a, Session& s) {
    std::string name = a.get_str("name");
    if (name.empty()) return ToolResult::fail("missing required parameter: name");

    auto configs = load_config();
    std::vector<std::string> index_names;
    std::string idx_arg = a.has("index") ? a.get_str("index") : s.default_index;
    if (!idx_arg.empty()) {
        index_names = split_csv(idx_arg);
    } else {
        for (auto& [n, cfg] : configs)
            if (cfg.is_indexed()) index_names.push_back(n);
    }

    int show_lines = a.has("lines") ? (int)a.get_int("lines") : 15;
    if (show_lines < 0) show_lines = 15;
    std::string file_filter = a.get_str("file");

    // Load call graphs.
    std::vector<CallGraph> graphs;
    std::vector<std::string> graph_names;
    for (auto& idx_name : index_names) {
        graphs.push_back(open_backend(idx_name)->load_call_graph());
        graph_names.push_back(idx_name);
    }

    // Definition lookup: exact/signature chunk match in the index entries
    // (same shape as cmd_explore), with a call-graph + metrics fallback.
    struct Definition { std::string index; std::string file; uint32_t line; uint32_t end_line; uint64_t hash; };
    std::vector<Definition> defs;
    for (auto& idx_name : index_names) {
        auto idx = open_backend(idx_name)->load_index();
        for (auto& e : idx.entries) {
            if (e.proto != EntryType::DOC) continue;
            bool exact = (e.chunk == name);
            bool sig = (e.chunk.size() > name.size() &&
                        e.chunk.substr(0, name.size()) == name &&
                        (e.chunk[name.size()] == '(' || e.chunk[name.size()] == ' '));
            if (!exact && !sig) continue;
            bool merged = false;
            for (auto& u : defs) {
                if (u.index == idx_name && u.file == e.path) {
                    if (sig) { u.line = e.line; u.end_line = e.end_line; u.hash = e.hash; }
                    merged = true; break;
                }
            }
            if (!merged) defs.push_back({idx_name, e.path, e.line, e.end_line, e.hash});
        }
    }
    if (defs.empty()) {
        for (size_t i = 0; i < graphs.size(); i++) {
            auto files = graphs[i].files_defining(name);
            auto mstore = open_backend(graph_names[i])->load_metrics();
            for (auto& f : files) {
                uint32_t line = 0, end_line = 0; bool from_metrics = false;
                for (auto& m : mstore.entries)
                    if (m.name == name && m.file == f) {
                        line = m.line; end_line = m.line + (m.lines > 0 ? m.lines - 1 : 0);
                        from_metrics = true; break;
                    }
                if (!from_metrics)
                    for (auto& e : graphs[i].edges)
                        if (e.caller == name && e.file == f) { line = e.line; break; }
                defs.push_back({graph_names[i], f, line, end_line, 0});
            }
        }
    }

    // Optional --file filter, then dedup by (index,file,line).
    if (!file_filter.empty()) {
        std::vector<Definition> filtered;
        for (auto& d : defs)
            if (d.file.find(file_filter) != std::string::npos) filtered.push_back(d);
        defs = std::move(filtered);
    }
    std::vector<Definition> uniq;
    for (auto& d : defs) {
        bool dup = false;
        for (auto& u : uniq)
            if (u.index == d.index && u.file == d.file && u.line == d.line) { dup = true; break; }
        if (!dup) uniq.push_back(d);
    }

    if (uniq.empty())
        return ToolResult::fail(
            "No top-level definition found for \"" + name + "\"" +
            (a.has("index") ? " in index " + a.get_str("index") : "") +
            ". explore matches top-level functions/types by name; methods, "
            "locals, and concepts won't match. Try `query \"" + name + "\"` to "
            "locate it semantically, then explore/get the result's location. "
            "Check the name spelling and that the right index is indexed.");

    if (uniq.size() > 1) {
        // Ambiguous — return the candidates so the agent can narrow with `file`.
        Json cands = Json::array();
        for (auto& d : uniq) {
            Json c = Json::object();
            c["index"] = d.index; c["file"] = d.file; c["line"] = d.line;
            cands.push_back(std::move(c));
        }
        Json out = Json::object();
        out["name"] = name;
        out["ambiguous"] = true;
        out["definitions"] = std::move(cands);
        out["hint"] = "multiple definitions — pass `file` (path substring) or `index` to disambiguate";
        return ToolResult::ok(std::move(out));
    }

    const Definition& def = uniq[0];

    Json out = Json::object();
    out["name"] = name;
    out["index"] = def.index;
    out["file"] = def.file;
    out["line"] = def.line;
    out["end_line"] = def.end_line;

    // Function length from metrics.
    int func_lines = 0;
    {
        auto mstore = open_backend(def.index)->load_metrics();
        for (auto& m : mstore.entries)
            if (m.name == name && m.file == def.file) { func_lines = m.lines; break; }
    }
    out["func_lines"] = func_lines;

    // Staleness (best-effort).
    bool stale = false;
    if (def.hash != 0 && configs.count(def.index)) {
        FileAdapter fa(def.index, configs.at(def.index));
        auto st = fa.check_stale(def.file, def.hash);
        if (st == EntryStatus::STALE || st == EntryStatus::MISSING) stale = true;
    }
    out["stale"] = stale;

    // Source snippet (via op_get, same as cmd_explore's print_source).
    if (!stale && def.line != 0 && configs.count(def.index)) {
        int lines_to_show = show_lines;
        if (lines_to_show == 0) {
            if (def.end_line > def.line) lines_to_show = def.end_line - def.line + 1;
            else if (func_lines > 0) lines_to_show = func_lines;
            else lines_to_show = 15;
        }
        std::string doc_path = def.file + ":" + std::to_string(def.line);
        auto res = op_get(EntryType::DOC, def.index, doc_path, configs.at(def.index), lines_to_show);
        if (res.ok) out["source"] = res.message;
    }

    // Callers (dedup by name+file), same as cmd_explore's print_callers.
    CallGraphQuery cg_query;
    cg_query.graphs = graphs;
    cg_query.graph_names = graph_names;

    {
        std::set<std::string> seen;
        Json callers = Json::array();
        for (size_t i = 0; i < graphs.size(); i++)
            for (auto& e : graphs[i].callers_of(name)) {
                std::string key = e.caller + "\t" + e.file;
                if (seen.count(key)) continue; seen.insert(key);
                std::string ix = cg_query.find_index_for(e.caller, e.file);
                if (ix.empty()) ix = def.index;
                Json c = Json::object();
                c["name"] = e.caller; c["file"] = e.file; c["index"] = ix; c["line"] = e.line;
                callers.push_back(std::move(c));
            }
        out["callers"] = std::move(callers);
    }

    // Callees — project-defined only (present as a caller somewhere), like
    // cmd_explore's print_callees.
    {
        std::set<std::string> all_callers_set;
        for (auto& g : graphs) for (auto& e : g.edges) all_callers_set.insert(e.caller);
        std::set<std::string> seen;
        Json callees = Json::array();
        for (size_t i = 0; i < graphs.size(); i++)
            for (auto& e : graphs[i].callees_of(name)) {
                if (seen.count(e.callee)) continue; seen.insert(e.callee);
                if (!all_callers_set.count(e.callee)) continue;
                std::string ix = cg_query.find_index_for(e.callee, e.file);
                if (ix.empty()) ix = def.index;
                Json c = Json::object();
                c["name"] = e.callee; c["file"] = e.file; c["index"] = ix; c["line"] = e.line;
                callees.push_back(std::move(c));
            }
        out["callees"] = std::move(callees);
    }

    // Imports — project-relative only (same filter as cmd_explore).
    {
        auto igraph = open_backend(def.index)->load_import_graph();
        Json imports = Json::array();
        for (auto& imp : igraph.imports_of(def.file))
            if (imp.size() >= 2 && imp[0] == '.' && (imp[1] == '/' || imp[1] == '.'))
                imports.push_back(imp);
        out["imports"] = std::move(imports);
    }

    // Doc references (markdown link graph) for .md/.mdx.
    {
        auto ends_with = [](const std::string& s, const std::string& suf) {
            return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
        };
        if (ends_with(def.file, ".md") || ends_with(def.file, ".mdx")) {
            auto graph = open_backend(def.index)->load_docref_graph();
            Json refs = Json::array(), ext = Json::array(), refby = Json::array();
            for (auto& e : graph.references_of(def.file)) {
                Json r = Json::object(); r["target"] = e.target; r["text"] = e.text;
                if (e.kind == RefKind::Local) refs.push_back(std::move(r));
                else if (e.kind == RefKind::External) ext.push_back(std::move(r));
            }
            for (auto& src : graph.referenced_by(def.file)) refby.push_back(src);
            out["references"] = std::move(refs);
            out["external_references"] = std::move(ext);
            out["referenced_by"] = std::move(refby);
        }
    }

    // Related (semantic) — best-effort, only when the daemon is up.
    Json related = Json::array();
    if (daemon_is_running()) {
        RpcClient rpc;
        if (rpc.connected()) {
            QueryRequest req;
            req.query = name; req.index = idx_arg; req.type_filter = -1; req.top_k = 5;
            std::vector<QueryResultEntry> results;
            if (rpc.cmd_query(req, results).empty()) {
                for (auto& r : results) {
                    if (r.path == def.file && r.line == def.line) continue;
                    Json j = Json::object();
                    j["index"] = r.index; j["path"] = r.path; j["line"] = r.line;
                    if (!r.chunk.empty()) j["chunk"] = r.chunk;
                    related.push_back(std::move(j));
                }
            }
        }
    }
    out["related"] = std::move(related);

    return ToolResult::ok(std::move(out));
}

// --- set_scope: set the session default index/type for subsequent tool calls
// (P4 sessions). Read-only — mutates only per-process session state, never a
// store. Delegates to apply_set_scope (pure, in the daemon-free registry TU) so
// the logic is shared with the unit tests.
ToolResult handle_set_scope(const ToolArgs& a, Session& s) {
    Json scope = apply_set_scope(s, a);
    return ToolResult::ok(std::move(scope));
}

// --- Write handlers (gated by [mcp] tools = read-write). Reuse RpcClient::
// cmd_add; write to gogol's own note/glossary stores (always writable). ---

ToolResult handle_add_note(const ToolArgs& a, Session& s) {
    RpcClient rpc;
    if (!rpc.connected())
        return ToolResult::fail("No gogol daemon running. Start it with: gogol serve");
    AddRequest req;
    req.type = EntryType::NOTE;
    req.index = a.has("index") ? a.get_str("index") : s.default_index;
    req.path = a.get_str("path");
    req.content = a.get_str("content");
    req.sources = a.has("sources") ? a.get_str("sources") : "";
    if (req.index.empty()) return ToolResult::fail("index required (or set_scope first)");
    std::string msg = rpc.cmd_add(req);
    // cmd_add returns the human message in BOTH cases: the "Added: ..." success
    // text, or an "Error: ..."/connection-failure string. Treat an Error:/known
    // failure prefix as failure; anything else is success.
    if (msg.rfind("Error:", 0) == 0 || msg == "Not connected" ||
        msg == "Send failed" || msg == "Read failed")
        return ToolResult::fail(msg);
    Json out = Json::object();
    out["added"] = "note"; out["index"] = req.index; out["path"] = req.path;
    out["message"] = msg;
    return ToolResult::ok(std::move(out));
}

ToolResult handle_add_term(const ToolArgs& a, Session& s) {
    RpcClient rpc;
    if (!rpc.connected())
        return ToolResult::fail("No gogol daemon running. Start it with: gogol serve");
    AddRequest req;
    req.type = EntryType::TERM;
    req.index = a.has("index") ? a.get_str("index") : s.default_index;
    req.path = a.get_str("path");       // the term
    req.content = a.get_str("content"); // the expansion
    if (req.index.empty()) return ToolResult::fail("index required (or set_scope first)");
    std::string msg = rpc.cmd_add(req);
    if (msg.rfind("Error:", 0) == 0 || msg == "Not connected" ||
        msg == "Send failed" || msg == "Read failed")
        return ToolResult::fail(msg);
    Json out = Json::object();
    out["added"] = "term"; out["index"] = req.index; out["term"] = req.path;
    out["message"] = msg;
    return ToolResult::ok(std::move(out));
}

// The SINGLE dispatch site: one name→handler table, built once. Adding a tool
// means adding its ToolDef to the registry (catalog/schema) and one entry here
// (behavior). tools/call resolves the handler through this — no scattered
// per-tool branching elsewhere.
ToolResult tool_dispatch(const std::string& name, const ToolArgs& args, Session& session) {
    static const std::unordered_map<std::string,
        std::function<ToolResult(const ToolArgs&, Session&)>> handlers = {
        {"query",    handle_query},
        {"get",      handle_get},
        {"list",     handle_list},
        {"explore",  handle_explore},
        {"calls",    handle_calls},
        {"affected", handle_affected},
        {"set_scope", handle_set_scope},
        {"add_note", handle_add_note},
        {"add_term", handle_add_term},
    };
    auto it = handlers.find(name);
    if (it == handlers.end()) return ToolResult::fail("unknown tool: " + name);
    return it->second(args, session);
}

// Find a tool def by name in the registry (nullptr if absent / write-gated).
const ToolDef* find_tool(const std::string& name, bool include_write) {
    for (const ToolDef& def : tool_registry()) {
        if (def.name != name) continue;
        if (!def.read_only && !include_write) return nullptr;
        return &def;
    }
    return nullptr;
}

// True if a tool with this name exists in the registry, regardless of the
// read/write gate (used to distinguish "unknown" from "write-gated").
bool tool_exists(const std::string& name) {
    for (const ToolDef& def : tool_registry())
        if (def.name == name) return true;
    return false;
}

// Validate that all required params are present (and non-empty for strings).
// Returns "" if valid, else a human-readable error naming the missing param.
std::string validate_required(const ToolDef& def, const ToolArgs& args) {
    for (const ToolParam& p : def.params) {
        if (!p.required) continue;
        if (!args.has(p.name))
            return "missing required parameter: " + p.name;
        // A required string present but empty is also invalid.
        if (p.type == "string" && args.get_str(p.name).empty())
            return "empty required parameter: " + p.name;
    }
    return "";
}

// --- Method handlers ---

Json handle_initialize(const Json& id) {
    Json server_info = Json::object();
    server_info["name"] = kServerName;
    server_info["version"] = kServerVersion;

    Json tools_cap = Json::object();  // {} — tools capability present
    Json capabilities = Json::object();
    capabilities["tools"] = std::move(tools_cap);

    Json result = Json::object();
    result["protocolVersion"] = kProtocolVersion;
    result["serverInfo"] = std::move(server_info);
    result["capabilities"] = std::move(capabilities);
    // Server instructions (MCP spec: optional guidance injected into the model's
    // context by clients that support it). Front-loads the cross-tool workflow —
    // which tool for which intent + the "don't re-query" rule — in the first
    // ~512 chars, per ecosystem best practice. Generic: no deployment specifics.
    result["instructions"] = kInstructions;
    return make_response(id, std::move(result));
}

Json handle_tools_list(const Json& id, bool include_write) {
    Json result = Json::object();
    result["tools"] = tools_list_schema(include_write);
    return make_response(id, std::move(result));
}

Json handle_tools_call(const Json& id, const Json& params, bool include_write,
                       Session& session) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
        return make_error(id, kInvalidParams, "tools/call requires a string 'name'");

    std::string name = params["name"].get<std::string>();
    const ToolDef* def = find_tool(name, include_write);
    if (!def) {
        // Distinguish a genuinely unknown tool from one that exists but is
        // write-gated under `tools = read` (defensive: refuse even if the
        // client knows the name).
        if (tool_exists(name))
            return make_error(id, kInvalidParams,
                              "tool '" + name +
                                  "' requires write access; set [mcp] tools = "
                                  "read-write in ~/.gogol/config to enable it");
        return make_error(id, kMethodNotFound, "unknown tool: " + name);
    }

    ToolArgs args;
    if (params.contains("arguments") && params["arguments"].is_object())
        args.obj = params["arguments"];
    else
        args.obj = Json::object();

    std::string verr = validate_required(*def, args);
    if (!verr.empty())
        return make_error(id, kInvalidParams, verr);

    // Single dispatch point: resolve the handler by name via tool_dispatch().
    // The registry validated existence + params + the read/write gate above;
    // execution is bound in one table (tool_dispatch) — adding a tool is its
    // ToolDef (catalog/schema) + one dispatch entry.
    ToolResult tr = tool_dispatch(name, args, session);
    return make_response(id, wrap_tool_result(tr));
}

// Process a single already-parsed JSON-RPC request object. `has_id` reports
// whether the message carries an id (notifications have none → no response).
// Returns true if a response should be written (via `out_response`).
bool process_request(const Json& msg, bool include_write, Session& session,
                     Json& out_response) {
    // id is echoed back; null for notifications / when absent.
    Json id = (msg.is_object() && msg.contains("id")) ? msg["id"] : Json(nullptr);

    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        out_response = make_error(id, kInvalidParams, "missing 'method'");
        return true;
    }

    std::string method = msg["method"].get<std::string>();
    Json params = msg.contains("params") ? msg["params"] : Json::object();

    // Notifications (no id) get no response per JSON-RPC 2.0.
    bool is_notification = !(msg.contains("id"));

    if (method == "initialize") {
        out_response = handle_initialize(id);
        return true;
    }
    if (method == "notifications/initialized" || method == "initialized") {
        // Client ack after initialize — a notification, no response.
        return false;
    }
    if (method == "tools/list") {
        out_response = handle_tools_list(id, include_write);
        return true;
    }
    if (method == "tools/call") {
        out_response = handle_tools_call(id, params, include_write, session);
        return true;
    }

    if (is_notification) return false;  // unknown notification: ignore silently
    out_response = make_error(id, kMethodNotFound, "unknown method: " + method);
    return true;
}

}  // namespace

int run_stdio_server(std::istream& in, std::ostream& out, bool include_write) {
    Session session;  // per-process session state (scope grows in P4)
    std::string line;
    while (std::getline(in, line)) {
        // Skip blank lines between messages.
        if (line.empty()) continue;

        Json msg;
        std::string parse_err;
        if (!json_parse(line, msg, &parse_err)) {
            Json resp = make_error(Json(nullptr), kParseError, "parse error: " + parse_err);
            out << resp.dump() << "\n";
            out.flush();
            continue;
        }

        Json response;
        bool have_response = false;
        try {
            have_response = process_request(msg, include_write, session, response);
        } catch (const std::exception& e) {
            Json id = (msg.is_object() && msg.contains("id")) ? msg["id"] : Json(nullptr);
            response = make_error(id, kInternalError, std::string("internal error: ") + e.what());
            have_response = true;
        }

        if (have_response) {
            out << response.dump() << "\n";
            out.flush();
        }
    }
    return 0;
}

}  // namespace mcp
}  // namespace gogol

int cmd_mcp() {
    // Config gating (P3): the MCP server is off unless explicitly enabled in
    // ~/.gogol/config. Refuse to start with a clear message BEFORE entering the
    // stdio loop. `tools = read-write` gates write tools; default is read-only.
    GlobalConfig gc = load_global_config();
    if (!gc.mcp_enabled) {
        std::cerr << "MCP server is disabled. Enable it with [mcp] enabled = true "
                     "in ~/.gogol/config\n";
        return 1;
    }
    const bool include_write = gc.mcp_read_write();
    return gogol::mcp::run_stdio_server(std::cin, std::cout, include_write);
}
