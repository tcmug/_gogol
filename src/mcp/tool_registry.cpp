#include "mcp/tool_registry.h"

namespace gogol {
namespace mcp {

namespace {

std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> tools;

    // query — semantic + keyword hybrid search with RRF ranking.
    tools.push_back(ToolDef{
        "query",
        "Semantic + keyword hybrid search. Find code by concept or meaning when "
        "you don't know the exact string. Returns ranked entries (leading type "
        "token + location) that are copy-pasteable into `get`/`explore`. If a "
        "query finds the right location, retrieve it — don't re-query with "
        "different terms (two queries max, then switch strategy).",
        {
            {"text", "string", "The search query — the intent/concept to find.", true, ""},
            {"index", "string", "Restrict to one index (comma-separated). Omit to search all.", false, ""},
            {"type", "string", "Filter by entry type: doc | note | term. Omit for all types.", false, ""},
            {"limit", "integer", "Max number of results to return.", false, "5"},
        },
        /*read_only=*/true,
    });

    // explore — one-call function/doc deep dive.
    tools.push_back(ToolDef{
        "explore",
        "One-call deep dive on a TOP-LEVEL function or type you can name: source "
        "snippet, file imports, callers, callees (project-only), and related "
        "symbols in a single ~2KB result. Replaces a query + get + calls "
        "sequence. For a doc/markdown entry it shows the reference graph instead. "
        "Matches top-level symbols by name — for a method, local, or concept, use "
        "`query` first, then explore/get the result. Use when you know the name.",
        {
            {"name", "string", "Top-level function or type (or doc) name to explore.", true, ""},
            {"index", "string", "Restrict to one index. Omit to search all.", false, ""},
            {"file", "string", "Disambiguate by file-path substring when a name has multiple definitions.", false, ""},
            {"lines", "integer", "Number of source lines to show.", false, "0"},
        },
        /*read_only=*/true,
    });

    // calls — AST-extracted call graph.
    tools.push_back(ToolDef{
        "calls",
        "AST-extracted call graph for a function — trace execution flow across "
        "files without reading them. Shows callers above and callees below; "
        "`depth` walks a recursive tree, `direction` restricts to one side.",
        {
            {"name", "string", "Function name to build the call graph for.", true, ""},
            {"index", "string", "Restrict to one index. Omit to search all.", false, ""},
            {"depth", "integer", "Recursive tree depth (1 = direct neighbours).", false, "1"},
            {"direction", "string", "Restrict to callers (in) or callees (out). Omit for both.", false, ""},
        },
        /*read_only=*/true,
    });

    // affected — transitive import-graph blast radius.
    tools.push_back(ToolDef{
        "affected",
        "Find the blast radius of a change: all files that transitively depend "
        "on the given files via the import graph. Use `filter` to narrow to "
        "e.g. test files; `depth` bounds the traversal.",
        {
            {"files", "array", "One or more file paths to compute dependents for.", true, ""},
            {"index", "string", "Restrict to one index. Omit to search all.", false, ""},
            {"filter", "string", "Glob to filter results (e.g. \"*.test.*\", \"e2e/**\").", false, ""},
            {"depth", "integer", "Bound the transitive traversal depth.", false, "0"},
        },
        /*read_only=*/true,
    });

    // get — direct retrieval by location, or by a result cursor.
    tools.push_back(ToolDef{
        "get",
        "Retrieve an entry's content by exact location. Use after query/list "
        "finds the right entry. A doc path may carry a `:line` suffix; `lines` "
        "limits how many lines are returned. Alternatively pass `result` (a "
        "0-based index into the last query/calls result set) to retrieve a hit "
        "without re-specifying type/index/path.",
        {
            {"type", "string", "Entry type: doc | note | term. Omit when using `result`.", false, ""},
            {"index", "string", "Index the entry lives in. Omit to use the session default or a `result` cursor.", false, ""},
            {"path", "string", "Entry path (doc may include a :line suffix). Omit when using `result`.", false, ""},
            {"result", "integer", "0-based index into the last query/calls result set. Resolves type/index/path from the cache.", false, ""},
            {"lines", "integer", "Max lines to return (0 = full).", false, "0"},
        },
        /*read_only=*/true,
    });

    // list — enumerate entries, or overview all indexes when none given.
    tools.push_back(ToolDef{
        "list",
        "With an `index`: enumerate its entries (optionally filtered by type) — "
        "use to see what docs/notes/terms exist before querying. With NO index: "
        "returns an overview of all available indexes and their entry counts "
        "(a good first call to discover what's indexed).",
        {
            {"index", "string", "Index to list. Omit for an overview of all indexes + counts.", false, ""},
            {"type", "string", "Filter by entry type: doc | note | term. Omit for all types.", false, ""},
        },
        /*read_only=*/true,
    });

    // set_scope — session default index/type for subsequent tool calls (P4).
    // Read-only: it mutates only per-process session state, never any store.
    tools.push_back(ToolDef{
        "set_scope",
        "OPTIONAL convenience: set a session default index/type so you can omit "
        "`index` on later calls. Only worth it for a long run against ONE index "
        "\xE2\x80\x94 otherwise just pass `index` inline (one field, no extra call). "
        "Omitted fields are left unchanged; returns the current scope.",
        {
            {"index", "string", "Default index for later calls. Omit to leave unchanged.", false, ""},
            {"type", "string", "Default entry type filter: doc | note | term. Omit to leave unchanged.", false, ""},
        },
        /*read_only=*/true,
    });

    // --- Write tools (read_only=false) — hidden/refused unless config
    // [mcp] tools = read-write. They write to gogol's own note/glossary stores
    // (always writable, independent of an index's r/rw mode). ---

    // add_note — create/update a memory note.
    tools.push_back(ToolDef{
        "add_note",
        "Store a memory note in an index's knowledge store (embedded so it's "
        "searchable). Use to persist a durable finding, decision, or gotcha so "
        "it isn't lost. Works on any index regardless of its read/write mode.",
        {
            {"index", "string", "Index whose store the note lives in.", true, ""},
            {"path", "string", "Note topic/path, e.g. \"auth/session-flow\".", true, ""},
            {"content", "string", "Note content (markdown).", true, ""},
            {"sources", "string", "Optional comma-separated source locations.", false, ""},
        },
        /*read_only=*/false,
    });

    // add_term — create/update a glossary term.
    tools.push_back(ToolDef{
        "add_term",
        "Add a glossary term (term -> expansion) to an index. Expansions enrich "
        "both indexing and query, so abbreviations resolve to their concept. "
        "Works on any index regardless of mode.",
        {
            {"index", "string", "Index whose glossary the term lives in.", true, ""},
            {"path", "string", "The term, e.g. \"OMS\".", true, ""},
            {"content", "string", "The expansion, e.g. \"order management system, ...\".", true, ""},
        },
        /*read_only=*/false,
    });

    return tools;
}

}  // namespace

const std::vector<ToolDef>& tool_registry() {
    static const std::vector<ToolDef> registry = build_registry();
    return registry;
}

Json tools_list_schema(bool include_write) {
    Json arr = Json::array();
    for (const ToolDef& def : tool_registry()) {
        if (!def.read_only && !include_write) continue;

        Json properties = Json::object();
        Json required = Json::array();
        for (const ToolParam& p : def.params) {
            properties[p.name] = Json{{"type", p.type}, {"description", p.description}};
            if (p.required) required.push_back(p.name);
        }

        Json input_schema = Json::object();
        input_schema["type"] = "object";
        input_schema["properties"] = std::move(properties);
        input_schema["required"] = std::move(required);

        Json tool = Json::object();
        tool["name"] = def.name;
        tool["description"] = def.description;
        tool["inputSchema"] = std::move(input_schema);

        arr.push_back(std::move(tool));
    }
    return arr;
}

}  // namespace mcp
}  // namespace gogol
