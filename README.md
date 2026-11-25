# gogol

Semantic code search using llama.cpp embeddings. Zero-config, daemon-less — just point it at a git repo.

Named after the digital mind-copies in Hannu Rajaniemi's *The Quantum Thief*.

## Build

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gogol -j$(nproc)
```

## Setup

Download [nomic-embed-text-v1.5](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF):

```bash
mkdir -p ~/.gogol/models
curl -L -o ~/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
```

Add to `~/.gogol/config`:
```ini
model = ~/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf
```

## Quick Start

```bash
cd ~/projects/myrepo

# Search (auto-indexes on first run, shows progress spinner)
gogol recall "auth token refresh"

# Deep-dive a function
gogol delve processOrder

# Call graph
gogol links handleEvent --kind call

# Find what depends on a file
gogol links src/utils.ts --kind import --direction in

# Rank functions by complexity
gogol metrics --limit 20

# Add a note (LLM-authored, persists across sessions)
gogol remember "The auth flow uses JWT tokens stored in sessionStorage"

# Explicit index (usually not needed — recall auto-indexes)
gogol index
```

## How It Works

**Zero-config**: Point gogol at any git repo. It auto-detects the repo boundary, indexes all supported files, and stores everything in `~/.gogol/cache.db`.

**Daemon-less**: No background process. The embedding model loads on first use (~2s cold start) and stays warm for the session.

**Worktree-aware**: Git worktrees share an index. Content indexed from main is visible from worktrees and vice versa. Each checkout only sees files that exist in its tree.

**Incremental**: Only changed files are re-embedded. Unchanged files are skipped via content-hash comparison.

## Commands

### `gogol recall <query>`
Semantic + keyword hybrid search. Auto-refreshes the index for the current git repo.

```bash
gogol recall "database connection pool"
gogol recall "error handling" --scope ~/other/repo   # another repo by path or name
gogol recall "auth" --ref e1bea64…                    # a repo by its root-commit SHA
```

### `gogol delve <symbol>`
Deep-dive a function: shows definition, source, callers, callees, and imports.

```bash
gogol delve processOrder
gogol delve handleEvent --scope backend              # scope to one repo by name/path
```

### `gogol links <node>`
Navigate the call or import graph.

```bash
gogol links handleEvent --kind call              # callers + callees
gogol links handleEvent --kind call --direction in   # callers only
gogol links src/utils.ts --kind import --direction in --depth 3  # transitive dependents
```

### `gogol metrics`
Rank functions by cyclomatic complexity.

```bash
gogol metrics --limit 20
gogol metrics --file orders.ts
```

### Scoping: `--scope` and `--ref`

Every read command (`recall`, `delve`, `links`, `literal`, `metrics`) scopes the same way
— two distinct selectors for two different jobs:

- **`--scope <path | name | dir>`** — which repo/dir you mean, by a human handle. A path
  (absolute/relative), a repo name (`backend`), or a parent directory holding several
  repos (unions them). Default is the CWD's repo. A path-shaped scope that doesn't exist
  on disk is an error (like `ls`), not a silent whole-store search.
- **`--ref <root-sha>`** — the same repo by its portable root-commit SHA (no filesystem).
  For a machine/container with no shared checkout. Get the SHA from `gogol status`
  (`index_list[].root_sha`).

`scope` and `ref` don't overlap: a SHA isn't a scope input, a path/name isn't a ref.
(The old `--index` flag was retired — `--scope` by name covers it.)

### `gogol remember <content>`
Store an LLM-authored note. Notes persist in `~/.gogol/notes.db` and surface in recall results.

```bash
gogol remember "The payment flow requires 3DS verification for EU cards"
```

Pass `--consolidate <id>` to replace an existing note: the old note is archived in-place (its `notes_vec` row is evicted so it disappears from recall), while the old row stays fetchable via `gogol get <id>`. A missing id or a code/doc id is reported (`forgotten`/`refused` in the output) and the new note is still stored.

```bash
gogol remember "Revised: 3DS is required only for non-SCA cards" --consolidate 257bac1a6182a727
```

### `gogol forget <id>`
Remove a note by its ID (from recall results).

### `gogol index [path]`
Explicitly index a repo. Usually not needed — `recall` auto-indexes.

```bash
gogol index              # index CWD repo
gogol index ~/projects/backend
```

### `gogol get <id>`
Fetch full content of an entry by ID.

### `gogol status`
Show store statistics as one JSON object: totals (entries, edges, indexes, notes) plus a
per-index `index_list`. Each entry carries the index identity (`idx`), its portable
`root_sha` (when the repo has a single root commit), the `entries` count, and `dirs` —
the on-disk checkout directory(ies), each with its own `overlay` flag (a worktree can be
an overlay while the main checkout isn't). A repo seen at several checkouts lists them all.

```bash
gogol status | jq '.store.index_list[] | {idx, dirs}'
```

### Agent mailbox (in-RAM, loopback-only)
`gogol serve` exposes three mailbox tools over the same HTTP MCP endpoint as the status page:

- `send` — hand off a message to another agent's inbox with `to`, optional `from`, and `body`. It returns `your_inbox`: the sender's own unread count at send time.
- `receive` — block until a message arrives for `me` (consume by default, wake instantly on send).
- `peek` — inspect the current inbox without consuming the message.

These mailboxes live only in the shared `gogol serve` process. They are intentionally ephemeral: a restart clears them. If an agent needs a message to survive a restart, it should record the durable fact in a `gogol remember` note and send a pointer/nudge instead. The mailbox glare rule is: if `send` returns `your_inbox > 0`, receive before you block — you already have mail waiting.

### `gogol mcp`
Run as an MCP server over stdio (for AI agents like Claude, Cursor, etc.).

### `gogol serve [--host 127.0.0.1 --port 7777]`
Run an HTTP MCP endpoint plus a local status dashboard.

- `POST /mcp` accepts the same JSON-RPC 2.0 requests as stdio.
- `GET /` serves a browser dashboard showing store totals, per-index breakdowns, and a recall box.
- Recall hits are clickable and open a detail view at `#/note/<id>` / `#/code/<id>` / `#/doc/<id>`; notes render as markdown and can be opened directly by hash.
- The default bind is loopback only (`127.0.0.1`) because this exposes recall + notes with no auth.
- Use explicit `--host 0.0.0.0` only if you intend to expose it beyond localhost; the server prints a warning.

```bash
# local-only status page + MCP endpoint
./build/gogol serve --port 7777

# browser (local machine only): http://127.0.0.1:7777/
# then query the same store over HTTP:
curl -s -XPOST http://127.0.0.1:7777/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"status"}}'

# open a rendered note directly in the browser
# http://127.0.0.1:7777/#/note/82f0165057df8fb6
```

## Output

Every command outputs a single JSON object (or value), pipeable to `jq`:

```bash
gogol recall "auth"   | jq '.results[].source'
gogol metrics         | jq '.functions[] | select(.complexity > 10) | .name'
gogol status          | jq '.store.index_list[].dirs'
```

Progress spinner (embedding status, ETA) goes to stderr, so it doesn't interfere with JSON output.

## Storage

```
~/.gogol/
├── config              # model path, settings
├── cache.db            # code/doc index (derived, rebuildable)
├── notes.db            # LLM-authored notes (durable, auto-backed up)
├── backups/            # daily snapshots of notes.db (Fibonacci retention)
│   └── YYYY-MM-DD/notes.db
└── models/
    └── nomic-embed-text-v1.5.Q4_K_M.gguf
```

- **cache.db**: All indexed code/docs, embeddings, call graph, metrics. Can be deleted and rebuilt.
- **notes.db**: Notes created via `remember`. Not auto-rebuilt — this is your data.
  gogol automatically backs up notes.db once per calendar day into `backups/YYYY-MM-DD/`,
  keeping a Fibonacci-aged set {1,2,3,5,8,13} days so an accidental overwrite is recoverable.

## Supported Languages

Tree-sitter parsing (function extraction, call graph):
- TypeScript/JavaScript (`.ts`, `.tsx`, `.js`, `.jsx`)
- Python (`.py`)
- Go (`.go`)
- Rust (`.rs`)
- C/C++ (`.c`, `.cpp`, `.cc`, `.h`, `.hpp`)
- PHP (`.php`)
- GDScript (`.gd`)

All text files are indexed for semantic search; the above get additional structure extraction.

## Neovim plugin

`nvim/` contains a Neovim picker for gogol (built on [snacks.nvim](https://github.com/folke/snacks.nvim)): a live semantic search that shows a cosine % per hit. Requires Neovim 0.10+ and `snacks.nvim`.

1. Build gogol (see [Build](#build)). The plugin uses `gogol` from `$PATH`, or falls back to `build/gogol` in this repo. To put it on `$PATH`:
   ```bash
   ln -sf "$PWD/build/gogol" ~/.local/bin/gogol   # ensure ~/.local/bin is on PATH
   ```
2. Add a [lazy.nvim](https://github.com/folke/lazy.nvim) spec (adjust `dir` to your checkout):
   ```lua
   {
     dir = vim.fn.expand("~/src/gogol/nvim"),
     name = "gogol.nvim",
     dependencies = { "folke/snacks.nvim" },
     cmd = "Gogol",
     keys = {
       { "<leader>sz", function() require("gogol").open() end, desc = "Search gogol (semantic, live)" },
       { "<leader>sZ", function() require("gogol").open_cword() end, desc = "Search gogol for word under cursor" },
     },
     config = function()
       require("gogol").setup()
     end,
   }
   ```
3. Restart Neovim (or `:Lazy sync`). Use `:Gogol [query]`, `<leader>sz`, or `<leader>sZ`.

Notes:
- Notes (`gogol remember`) appear in the list with their full text in the preview. Confirm opens them in a read-only scratch buffer (saving is not supported).
- Queries wait 500 ms after the last keystroke (on top of snacks' 200 ms). Change `GOGOL_DEBOUNCE_MS` in `nvim/lua/gogol.lua`.

## MCP Integration

For AI agents (Claude Code, Cursor, Codex), run gogol as an MCP server:

```bash
gogol mcp
```

Exposes tools: `recall`, `delve`, `links`, `metrics`, `remember`, `forget`, `get`, `status`.
