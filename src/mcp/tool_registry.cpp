#include "mcp/tool_registry.h"

namespace gogol {
namespace mcp {

namespace {

std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> tools;

    // NOTE: v1 index-backed tools (query/read/list/explore/calls/affected/write/
    // rm/move/metrics) were RETIRED — the v1 stores are migrated to the unified
    // memory. The v1 handlers have been removed from mcp_server.cpp; they are
    // neither dispatched nor advertised.


    // --- Unified memory store (~/.gogol/cache.db + notes.db).
    // Cross-index by default; hybrid semantic + call-graph retrieval. ---
    tools.push_back(ToolDef{
        "recall",
        "Search the v2 unified memory (code + docs + notes across ALL indexes in "
        "one store). Hybrid retrieval: semantic top-k PLUS call-graph expansion — "
        "results marked via_edge=true were reached through the call graph, not "
        "text similarity (structurally-related code a plain search misses). "
        "Cross-index by default; no index needs choosing.",
        {
            {"text", "string", "Natural-language / code query.", true, ""},
            {"limit", "integer", "Max results (default 10).", false, "10"},
            {"scope", "string", "Optional: scope to a git repo by a full path (/Users/.../web), a repo name (web) matching any path ending with that name, or a directory holding several repos (scopes to the repos under it). A path/name scope also triggers on-demand refresh of changed files and surfaces notes attached to this repo. For a portable, filesystem-independent identity (e.g. from a container with no shared filesystem) use `ref` instead.", false, ""},
            {"ref", "string", "Optional: scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — a portable identity needing no shared filesystem. Read-only lookup against an already-indexed repo; triggers NO refresh and applies no checkout filter. Discover a repo's ref via the status tool (index_list[].root_sha).", false, ""},
            {"type", "string", "Optional: filter to entry type 'note'|'doc'|'code' (e.g. type=note finds LLM-authored notes, which content search can't isolate).", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "status",
        "v2 memory store stats: entry / edge / index / note counts, PLUS a per-index list (index_list) giving each index's path, entry count, and root_sha (the portable ref to pass as a hash `scope` from a machine with no shared filesystem).",
        {},
    });
    tools.push_back(ToolDef{
        "get",
        "Fetch a single memory entry's full content by id (ids come from "
        "recall results). Returns content, type, source, tags.",
        {
            {"id", "string", "Entry id (from a recall result).", true, ""},
        },
    });
    tools.push_back(ToolDef{
        "delve",
        "Deep-dive a symbol: its definition/source plus callers, callees, and "
        "imports in one call (composed from the call/import graph + the entry "
        "store). Use when you already know the symbol name and want its full "
        "structural context, rather than searching by concept (recall).",
        {
            {"symbol", "string", "Symbol name to delve into (e.g. 'processOrder').", true, ""},
            {"scope", "string", "Optional: scope to one repo — a repo name (e.g. 'backend') or a full path — to avoid cross-index collisions on generic symbol names. Omit for all indexes. For a portable SHA identity use `ref`.", false, ""},
            {"ref", "string", "Optional: scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — a portable identity needing no shared filesystem. Discover a repo's ref via the status tool (index_list[].root_sha).", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "links",
        "Unified graph navigation over the extracted edge graph — the one tool for "
        "code AND doc structure. Edge kinds: 'call' (function->function), 'import' "
        "(file->file), Replaces the old separate tools: "
        "callees = kind=call direction=out; callers = kind=call direction=in; "
        "'affected' (reverse import deps) = kind=import direction=in depth=N. "
        "depth>1 walks transitively (cycle-safe).",
        {
            {"node", "string", "Symbol (e.g. 'processOrder') or doc path (e.g. 'docs/api.md').", true, ""},
            {"kind", "string", "Edge kind: 'call' | 'import'. Omit for all kinds.", false, ""},
            {"direction", "string", "'out' (node is source), 'in' (node is target), or 'both' (default).", false, "both"},
            {"depth", "integer", "Transitive hops to traverse (default 1). depth=N + kind=import + direction=in = 'affected'.", false, "1"},
            {"scope", "string", "Optional: scope to one repo — a repo name (e.g. 'backend') or a full path — to avoid cross-index collisions on generic names like 'main'/'get'. Omit for all. For a portable SHA identity use `ref`.", false, ""},
            {"ref", "string", "Optional: scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — a portable identity needing no shared filesystem. Discover it via the status tool (index_list[].root_sha).", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "metrics",
        "Rank indexed functions by cyclomatic complexity (a real code-quality "
        "signal from static analysis, not embeddings) — refactor candidates. "
        "Returns complexity, lines, params, returns, max nesting depth per function.",
        {
            {"limit", "integer", "Max functions (default 20).", false, "20"},
            {"filter", "string", "Optional path/file substring to scope to.", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "remember",
        "Author a note-document into the unified memory (LLM is the source of "
        "truth; stored with no external source, type=note). Use for durable "
        "findings/decisions/rationale — a proper document, any length, not just a "
        "scratch fact. It joins the same semantic space as code+docs, so "
        "classification and search find it alongside related code. Do NOT use for "
        "code/docs that live on disk — those are ingested from files.",
        {
            {"content", "string", "The note document (markdown, any length).", true, ""},
            {"namespace", "string", "Optional index tag namespace (e.g. a project); omit for none.", false, ""},
            {"scope", "string", "Optional path this note is attached to (e.g. a repo toplevel or subdir). The note then surfaces only for queries scoped to that path or a descendant of it. Omit for a global note (surfaces everywhere).", false, ""},
            {"consolidate", "string", "Optional id of an existing note to replace: it is archived in-place while the new note is written (no window where the note is gone from recall). Use when revising a note you previously remembered. Best-effort: a missing id or a code/doc id is reported (forgotten/refused flags) and the new note is still created. Identical content yields the same id (no-op update).", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "forget",
        "Remove an LLM-authored note from memory by id (ids come from recall "
        "results). Only forgets notes (things you remembered); refuses to remove "
        "code/doc entries, which live on disk (delete the file instead).",
        {
            {"id", "string", "Note entry id to forget (from a recall result).", true, ""},
        },
    });
    tools.push_back(ToolDef{
        "related",
        "Find entries related to a given entry by embedding similarity. Given an "
        "entry id (from recall results), returns other code/doc entries ranked by "
        "semantic similarity. Use to discover: which code implements a doc, which "
        "docs describe a piece of code, or what else is semantically related. "
        "Notes are excluded by default; use type='note' to find related notes.",
        {
            {"id", "string", "Entry id to find related entries for (from a recall result).", true, ""},
            {"limit", "integer", "Max results (default 10).", false, "10"},
            {"type", "string", "Optional: filter to 'code', 'doc', or 'note'. Empty returns code+doc.", false, ""},
        },
    });
    tools.push_back(ToolDef{
        "literal",
        "Literal phrase search — find entries containing an exact string. Use for "
        "known symbols, function names, error messages, config values. Faster and "
        "deterministic vs recall's semantic ranking. Returns file, line, symbol.",
        {
            {"pattern", "string", "Exact phrase to search for (e.g. 'useAddItemsToCart', 'TODO:', 'DATABASE_URL').", true, ""},
            {"limit", "integer", "Max results (default 50).", false, "50"},
            {"scope", "string", "Optional: scope to one repo — a repo name (e.g. 'backend') or a full path. Omit for all. For a portable SHA identity use `ref`.", false, ""},
            {"ref", "string", "Optional: scope to a repo by its root commit hash (git rev-list --max-parents=0 HEAD) — a portable identity needing no shared filesystem. Discover it via the status tool (index_list[].root_sha).", false, ""},
        },
    });

    // --- combadge agent message bus (persisted in messages.db, ADR-003) ---
    // listen + send are the agent-facing messaging surface over MCP: advertised only when
    // `[mcp] messaging = true`. Identity is an explicit handle (stateless MCP, SEP-2567):
    // listen returns `me`; the caller passes it back on later calls.
    tools.push_back(ToolDef{
        "listen",
        "Claim an agent name for messaging. Returns `me`: pass it as the `me` argument of "
        "later gogol calls (or let your client do it) to send as this name and to see a "
        "hint when messages are waiting for it.",
        {
            {"name", "string", "Agent name to listen as (e.g. 'bridge').", true, ""},
        },
        /*hidden=*/false,
        /*messaging=*/true,
    });
    tools.push_back(ToolDef{
        "send",
        "Send a message to another agent's inbox (addressed, hand-off delivery). Use to "
        "delegate a task or reply to one. Optionally attach semantic context: with "
        "with_context=true, gogol runs a recall for the body against the recipient's repo "
        "(scoped by ref) and ships the top hits WITH the message, so the recipient arrives "
        "pre-loaded and skips its own search turns.",
        {
            {"to", "string", "Recipient agent name.", true, ""},
            {"body", "string", "Message text.", true, ""},
            {"me", "string", "Your agent name from listen; the message is sent as this name.", false, ""},
            {"from", "string", "Sender agent name when `me` is not given (default 'anon').", false, "anon"},
            {"with_context", "boolean", "Attach semantic recall hits for the body to the message.", false, "false"},
            {"ref", "string", "Repo root-commit SHA to scope the with_context recall (portable identity; needs no shared filesystem).", false, ""},
            {"context_limit", "integer", "Max context hits to attach when with_context (default 5).", false, "5"},
        },
        /*hidden=*/false,
        /*messaging=*/true,
    });
    tools.push_back(ToolDef{
        "receive",
        "Block until a message addressed to you arrives (or timeout). Consumes and returns "
        "the oldest queued message. Long-poll: re-call on timeout.",
        {
            {"me", "string", "Your agent name (the inbox to receive from).", true, ""},
            {"timeout_ms", "integer", "Max block in ms (clamped to a server cap; default cap).", false, "0"},
            {"consume", "boolean", "Remove the message (true, default) or return a snapshot of all queued without consuming (false).", false, "true"},
            {"session", "string", "Listen-session id. When set, the message is claimed (redelivered until ack) and other sessions' claims for this name are requeued. Omit to consume on receive.", false, ""},
        },
        /*hidden=*/true,
    });
    tools.push_back(ToolDef{
        "ack",
        "Confirm messages received with a session id were handled, so they are not redelivered.",
        {
            {"me", "string", "Your agent name.", true, ""},
            {"seq", "array", "Seq numbers to ack (an integer or an array of integers).", true, ""},
        },
        /*hidden=*/true,
    });
    tools.push_back(ToolDef{
        "peek",
        "List queued messages for a name without consuming them (default: your own inbox).",
        {
            {"me", "string", "Inbox to peek.", true, ""},
        },
        /*hidden=*/true,
    });
    tools.push_back(ToolDef{
        "presence",
        "Set or query whether an agent name is currently listening. Pass online=true/false "
        "to set it; omit online to just query. Advisory — lets send report delivered_live "
        "before a receive is parked.",
        {
            {"name", "string", "Agent name.", true, ""},
            {"online", "boolean", "Set listening state; omit to query.", false, ""},
        },
        /*hidden=*/true,
    });

    return tools;
}

}  // namespace

const std::vector<ToolDef>& tool_registry() {
    static const std::vector<ToolDef> registry = build_registry();
    return registry;
}

Json tools_list_schema(bool messaging) {
    Json arr = Json::array();
    for (const ToolDef& def : tool_registry()) {
        if (def.hidden) continue;  // transport primitives (e.g. message-bus tools) stay callable but unadvertised
        if (def.messaging && !messaging) continue;  // messaging tools only with [mcp] messaging
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
