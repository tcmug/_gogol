# gogol MCP Server — Design

Status: DRAFT for review. 2026-08-31. No code yet.

## Goal

Expose gogol's operations as an MCP (Model Context Protocol) server so agents
(Claude Code, Cursor, Codex) call `query`/`explore`/`calls`/`affected`/`get`
as native tools instead of shelling out to the CLI. Constraints from the outset:

1. **Change-resilient** — adding/changing a tool must NOT require editing 3+
   places. One declaration drives everything.
2. **Config-gated** — off unless enabled in `~/.gogol/config`.
3. **Session-aware** — support MCP sessions (per-client state: default index,
   result cursors, follow-up context).

## The core problem: avoid triplicate tool definitions

Today a "command" is defined in THREE disconnected places:
- CLI arg parsing (`main.cpp`, CLI11)
- RPC wire encode/decode (`embed_client.cpp` ↔ `embed_server.cpp`, hand-packed)
- (new) MCP tool schema + dispatch

If MCP is bolted on as a 3rd hand-maintained copy, every tool change = edit all
three, and they drift. That is the "redo it every time" trap. The design must
collapse these to ONE source of truth.

## Architecture: a Tool Registry (single source of truth)

Define each gogol operation ONCE as a `ToolDef` and derive every surface from it.

```cpp
struct ToolParam {
  std::string name;
  std::string type;        // "string" | "integer" | "boolean"
  std::string description; // used verbatim in the MCP JSON Schema
  bool required = false;
  std::string default_val;
};

struct ToolDef {
  std::string name;                 // "query", "explore", ...
  std::string description;          // drawn from SKILL decision-flow prose
  std::vector<ToolParam> params;
  bool read_only = true;            // write tools gated separately
  // The single handler: params (parsed) -> structured result.
  // Runs against the daemon via the existing RpcClient (reuse, don't reimplement).
  std::function<ToolResult(const ToolArgs&, Session&)> handler;
};

const std::vector<ToolDef>& tool_registry();   // the ONE catalog
```

From this ONE registry we derive:
- **MCP `tools/list`** — generate JSON Schema from `params` automatically.
- **MCP `tools/call`** — look up by name, validate params against the def, call
  `handler`.
- (future) **CLI help / a `gogol tools` introspection command** — same catalog.
- Result formatting — the handler returns a structured `ToolResult`; the MCP
  layer serializes it to MCP content blocks. The existing CLI formatting stays
  separate (terminal vs agent) — MCP gets its own JSON serialization of the SAME
  structured result the daemon already returns.

Adding a tool = append one `ToolDef` to the registry. No wire changes (it reuses
the existing RpcClient calls), no schema hand-writing (generated), no dispatch
switch (registry lookup). THAT is the change-resilience.

## Layering (each swappable independently)

```
Agent (Claude/Cursor)  ⇄ stdio JSON-RPC 2.0 (MCP) ⇄  `gogol mcp`
                                                        │
   MCP protocol layer (initialize / tools/list / tools/call / sessions)
                                                        │
   Tool Registry (ToolDef[]) — single source of truth
                                                        │
   Handlers → existing RpcClient → unix socket → `gogol serve` daemon
```

`gogol mcp` is a THIN, mostly-stateless front-end process (like the CLI client):
it connects to the running daemon and translates MCP calls into existing RPC
calls. All heavy state stays in the daemon. This means the MCP layer never
duplicates search/graph logic — it's protocol glue over the registry.

## JSON dependency

MCP is JSON-RPC 2.0. gogol has no JSON lib. Vendor a single small header
(candidate: a minimal single-header JSON lib compiled as source, consistent with
the `sqlite3.c`/`monocypher.c` vendoring ethos). MCP messages are simple/flat, so
a compact parser suffices — do NOT pull a heavy dependency. Decision pending:
vendor a tiny header vs. hand-roll a minimal JSON reader/writer for the fixed
MCP message shapes. Lean: vendor a small, permissively-licensed single-header.

## Transport

- **v1: stdio** (newline-delimited JSON-RPC), the default for local agent tools.
- The MCP spec explicitly notes the same wire format works over Unix sockets/TCP
  unchanged — so a future `--tcp` MCP transport is a drop-in (reuse the existing
  socket plumbing). Design the transport behind a small interface so stdio vs
  socket is swappable.

## Config gating

MCP is OFF unless enabled. Add to `GlobalConfig`:

```ini
[mcp]
enabled = true            ; default false — feature is opt-in
tools = read             ; read | read-write  (write tools gated here)
default_index =          ; optional: sessions start scoped to this index
```

`gogol mcp` refuses to start (clean message) if `[mcp] enabled` is not true.
`read-write` is required before any add/rm tool is exposed in `tools/list` — so a
default install exposes read-only tools even if someone runs `gogol mcp`.

## Sessions

MCP supports per-connection session state. gogol sessions carry:
- **default index / type scope** — set once ("work in backend"), subsequent
  tool calls inherit it unless overridden. Removes repetitive `--index` args.
- **result cursors** — a `query`/`calls` returns a result set + an opaque cursor;
  a follow-up `get_more`/`explore_result[n]` resolves against the cached set
  without re-querying. (Mirrors the SKILL "if query found it, get/explore it —
  don't re-query" rule, enforced structurally.)
- **conversation-scoped notes** — optional: a session can accumulate findings
  that a `session_summary` tool can flush to a real note.

Session state lives in the `gogol mcp` process (one process per client connection
is the MCP norm), keyed by the MCP session id from `initialize`. It's small and
ephemeral — NOT persisted unless explicitly flushed to a note. The daemon stays
session-agnostic (sessions are a front-end concern), preserving the daemon's
stateless-per-request model.

## Tool catalog (v1, read-only)

Descriptions drawn from SKILL.md decision-flow (already written for LLM consumption):
- `query(text, index?, type?, limit?)` — semantic+keyword search.
- `explore(name, index?, file?, lines?)` — function/doc deep-dive.
- `calls(name, index?, depth?, direction?)` — call graph.
- `affected(files[], index?, filter?, depth?)` — blast radius.
- `get(type, index, path, lines?)` — retrieve by location.
- `list(index?, type?)` — enumerate entries.
- `set_scope(index?, type?)` — session default (sessions feature).

v2 (gated `tools = read-write`): `add_note`, `add_term`, `session_summary`.

## Change-resilience checklist (the litmus test)

Adding/changing a tool must touch ONLY:
1. its `ToolDef` in the registry (name, params, description, handler).
That's it. `tools/list` schema, param validation, dispatch, and (if it maps to an
existing RPC) the transport are all derived. New underlying capability may need a
daemon RPC, but the MCP surface itself is one struct.

## Build / phases

- P0: vendor minimal JSON; `Json` value + parse/serialize; unit tests.
- P1: `ToolDef`/registry + generate `tools/list` schema from it; unit test the
  schema generation.
- P2: `gogol mcp` stdio JSON-RPC loop: `initialize` + `tools/list` + `tools/call`
  dispatch through the registry; handlers call the existing RpcClient.
- P3: config gating (`[mcp] enabled`); read-only tool set.
- P4: sessions (scope + result cursors).
- P5: write tools behind `tools = read-write`; optional TCP transport.

Each phase independently buildable + tested; checkpoint commit per phase.

## Explicitly deferred / non-goals
- Cross-encoder reranking (separate relevance feature).
- MCP "resources"/"prompts" capabilities (tools-only v1).
- Multi-vector embeddings.
