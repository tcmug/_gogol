---
name: gogol
description: >
  Semantic code search and knowledge store backed by local embeddings, exposed as
  MCP tools namespaced `mcp__gogol__*`: recall, get, delve, links, metrics,
  related, literal, remember, forget, status. Use when: finding code by meaning
  (not grep), tracing function call/import relationships, ranking refactor
  candidates by cyclomatic complexity, storing durable findings/decisions as
  notes, and resuming a prior session in the same repo via a scoped session note.
  Explicit triggers: search code, explore call graphs, store a finding, resume
  session. Do NOT load for generic coding queries that need neither semantic
  search nor the cross-session note store.
user_invocable: true
---

# gogol

Kind: reference

Semantic search + knowledge store, local embeddings, exposed as MCP tools
`mcp__gogol__<tool>`. Params are JSON keys, not CLI flags (`scope="web"`,
never `--scope web`). Zero-config, daemon-less — auto-indexes a git repo on
first scoped `recall` (`indexing: true` = background refresh still running).
Optional params omitted → sensible defaults.

## Tools & decision flow

Pick by what you already know. **Searching code → a gogol tool, not `bash`
grep/rg/find/sed** — gogol has semantic, FTS5, and AST call-graph indexes
grep can't reach; grep returns flat text with no structure, related entries,
call graph, or notes. (The tell you're grepping instead of picking a gogol
tool: an alternation regex, or scanning a file list to `read` each one.)

| You know... | Tool | Not (bash) |
|---|---|---|
| Exact string/symbol (`move_frame_origin`, `DATABASE_URL`, `TODO:`) | `literal` | `grep -rn`, `rg` |
| Function/symbol name + want def, callers, callees, imports | `delve` | `grep` then `read` each |
| Concept/meaning ("payment capture flow"), or an alternation (`foo\|bar`) | `recall` | `grep -E` |
| Callers / callees / transitive import deps of a known symbol or file | `links` | `grep` for the name |
| Code/docs similar to a `recall`/`delve` result you have (its `id`) | `related` | a new `recall` with different words |
| Refactor candidates by cyclomatic complexity | `metrics` | eyeballing files |
| Read a found entry's full content (have its `id`) | `get` | — |

**Key rule:** if `recall` found the right location, `delve` or `get` it —
don't query again with different terms.

**`bash` is still correct for** (not code search): non-code text (logs,
`.gitignore`, config), working-tree/git state gogol can't see (`git status`/
`git diff`, uncommitted edits — verify a hit's on-disk content with `read`),
git ops, running scripts, `ls`, `wc`. Reading a known file → `read`.

## Common params

- **`scope`** — scope to a git repo. Repo *name* (`web` matches any indexed
  checkout path ending `/web`) or full path, or a parent dir (unions the repos
  under it). Also triggers on-demand refresh + surfaces notes attached to that
  path. (A path-shaped scope that doesn't exist errors, like `ls`.)
- **`ref`** — the same target by its portable root-commit SHA (no filesystem).
  For a container/machine with no shared checkout. Get it from `status`
  (`index_list[].root_sha`). `scope` (name/path) and `ref` (SHA) are distinct:
  a SHA isn't a scope, a name/path isn't a ref. (The old `index` param was
  retired — `scope` by repo name covers cross-index name collisions.)
- **`type`** — `note` | `doc` | `code`; omit for all. **`literal` can't find
  notes** (FTS5 is on cache.db; notes live in notes.db) — use
  `recall` with `type="note"`.

## Tool details

**recall** — hybrid: 0.6·cosine + 0.4·bm25_norm, then call-graph expansion.
Output: `score` (blend, for order), `cosine`/`max_cosine` (raw, for
confidence), `low_confidence`, `via_edge` (true = reached via call graph,
not text), `indexing`. **Trust `cosine`/`max_cosine`, not `score`** — but a
low cosine can still be the right hit when scoped+type-filtered; verify from
the snippet (e.g. a `# session YYYY-MM-DD` header) not the score.
```jsonc
mcp__gogol__recall({ "text": "payment capture flow" })                          // all indexes
mcp__gogol__recall({ "text": "auth token refresh", "scope": "web", "limit": 10 })// one repo
mcp__gogol__recall({ "text": "config", "type": "doc" })                         // docs only
mcp__gogol__recall({ "text": "session", "type": "note" })                       // notes only
```

**delve** — one call: full source + callers + callees + imports. Use when
you know the function name.
```jsonc
mcp__gogol__delve({ "symbol": "processOrder" })
mcp__gogol__delve({ "symbol": "handleEvent", "scope": "backend" })
```

**literal** — exact phrase (FTS5). Faster/deterministic vs `recall`. Returns
file, line, enclosing symbol. **Can't find notes.**
```jsonc
mcp__gogol__literal({ "pattern": "useAddItemsToCart" })
mcp__gogol__literal({ "pattern": "DATABASE_URL" })
mcp__gogol__literal({ "pattern": "TODO:", "limit": 20 })
```

**links** — AST call/import graph. `node` = symbol (`processOrder`) for call
edges, or file path (`docs/api.md`) for import edges. `kind` `call`|`import`.
`direction` `in`|`out`|`both` (default). `depth=N` + `kind=import` +
`direction=in` = "affected" (reverse import deps).
```jsonc
mcp__gogol__links({ "node": "handleEvent", "kind": "call" })                        // callers + callees
mcp__gogol__links({ "node": "handleEvent", "kind": "call", "direction": "in" })    // callers only
mcp__gogol__links({ "node": "src/utils.ts", "kind": "import", "direction": "in", "depth": 3 }) // affected
```

**related** — embedding-similar entries to a known one (have its `id`).
`type` `code`|`doc`|`note`; omit for code+doc.
```jsonc
mcp__gogol__related({ "id": "<id>", "type": "code" })   // code related to this entry
mcp__gogol__related({ "id": "<id>", "type": "doc" })    // docs describing this code
mcp__gogol__related({ "id": "<id>", "type": "note" })   // related notes
```

**metrics** — rank functions by cyclomatic complexity (refactor candidates).
Default excludes vendor/node_modules; `filter` (path substring) to scope.
```jsonc
mcp__gogol__metrics({ "limit": 20 })
mcp__gogol__metrics({ "filter": "web/src", "limit": 10 })
```

**get** / **status** — `get({ "id": "<id>" })` fetches full entry content;
`status({})` returns store stats.

## Notes (knowledge store)

LLM-authored content of record — durable, not scratch. Join the same
semantic space as code+docs. Surface in `recall` when query `scope`
path-prefix-matches the note's scope (omit `scope` for global). **Notes are
not rebuildable from code** — never overwrite/delete the store to test; the
binary auto-backs up notes.db daily (Fibonacci-aged {1,2,3,5,8,13} days) as
a *recovery* mechanism, not a dev workflow.
```jsonc
mcp__gogol__remember({ "content": "auth uses JWT in sessionStorage" })                  // global
mcp__gogol__remember({ "content": "3DS for EU cards", "scope": "/abs/path/to/repo" })  // scoped
mcp__gogol__forget({ "id": "<id>" })                                                   // remove
mcp__gogol__remember({ "content": "revised", "consolidate": "<old-id>" })              // replace atomically
```

## CLI (secondary)

A `gogol` binary exists for shell use (same 10 tools, flag syntax:
`--scope`, `--ref`, `--type`, `-n`/`--limit`, positional arg). Also has
`gogol index <path>` to pre-warm and `gogol serve` for a local HTTP MCP
transport + browser dashboard. **Prefer the MCP tools** — agent-native,
no shell overhead.

```bash
gogol serve --host 127.0.0.1 --port 7777  # local-only HTTP MCP + status page
```

## Session resume (cross-session state)

git carries ~60% (landed code, better than any note); a scoped session note
carries the ~40% git can't see (non-commit actions, open items, lessons).
At session end write ONE note scoped to the repo (`scope` = `git rev-parse
--show-toplevel`), in this exact shape (the shape is the contract — anchors
are greppable):

```
# session YYYY-MM-DD — <one-line repo state>
Supersedes: <prior-session-note-id, or none>

## Landed (see git log <base>..HEAD)
<highlights only — one or two lines. git is authoritative; don't re-list every commit.>

## Non-commit state (NOT in git)
- <recoveries, filesystem actions, other-repo changes, anything git can't see>

## Open
- <item> → next: <concrete next step>

## Lesson
- <omit if none; required after a prod incident or genuine surprise>
```

**Resume:** find the current (unsuperseded) note. **Use `recall`, not
`literal`** — notes have no FTS index; `recall`'s semantic arm hits
`notes_vec`. ONE scoped query with the full anchor (don't run unscoped
first, don't weaken it):
```jsonc
mcp__gogol__recall({ "text": "session Supersedes none", "scope": "/abs/path/to/repo", "type": "note" })
mcp__gogol__get({ "id": "<id>" })   // then read the full note
```
Session notes often score low cosine (`low_confidence: true`, `max_cosine
~0.5`) because the anchor is short — trust a hit whose snippet starts with
`# session YYYY-MM-DD` regardless of score. **Then stop investigating and
resume:** read the note, summarize its state (Landed/Non-commit/Open/Lesson)
to the user, ask which open item to tackle. The note IS the resume — don't
audit it. No git reconciliation, no reading untracked files, no hunting
supersession chains. Resume = find, read, summarize, ask. Anything beyond
that is the spaz.

**Lifecycle:** when you write a new session note, `Supersedes: <old-id>` it
and replace the prior with `remember(..., consolidate: "<old-id>")` — so "current
state" = the one note with `Supersedes: none`. The old note is archived, not
hard-deleted: `recall` hides it because its `notes_vec` row is removed, but `get`
still resolves the old id and the chain remains walkable.
```jsonc
mcp__gogol__remember({ "content": "<new session note>", "consolidate": "<old-id>" })
```
