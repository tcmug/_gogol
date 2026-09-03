# gogol

Semantic search and knowledge store using llama.cpp embeddings. Named after the digital mind-copies in Hannu Rajaniemi's *The Quantum Thief*.

CLI binary: `gogol`

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
wget -O ~/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
```

Q4_K_M is recommended (80MB, fast, negligible quality loss). For maximum precision use Q8_0 (139MB):
```bash
wget -O ~/.gogol/models/nomic-embed-text-v1.5.Q8_0.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q8_0.gguf
```

Add to `~/.gogol/config`:
```ini
model = ~/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf
```

## Quick Start

```bash
# Start the daemon (keeps model in memory for fast operations)
gogol serve

# Index all configured directories
gogol index

# Search
gogol query "auth token refresh"

# Search filtered to one entry type
gogol query "payment" --type doc

# Add a memory note
gogol add note web auth/flows "OAuth redirects to /login page"

# Retrieve
gogol get note web auth/flows

# List entries
gogol list web
gogol list note web

# Call graph
gogol calls "handleEvent" --index backend

# Explore a function (one-call deep dive)
gogol explore "processOrder" --index backend

# Find affected files
gogol affected "order-hash.ts" --index backend --filter "*.test.*"

# Stop daemon
gogol serve --stop
```

## Entry Types

Every entry is addressed as `<type> <index> <path>`. Three types:

| Type | Description | Example |
|------|-------------|---------|
| `doc` | File chunk under an index's configured paths | `doc web src/utils.ts:42 § parseConfig` |
| `note` | Memory note stored in the index's store | `note team deploy/kubernetes` |
| `term` | Glossary term (term → expansion) | `term backend OMS` |

`add`, `get`, and `rm` take the type as their first argument. `list` and `query` take an optional `--type` (or list type positional) to filter; omitted means all types.

## Commands

### `gogol serve`

Persistent daemon that holds the model + all indexes in memory. All other commands connect to it automatically. The daemon is required — commands that read or mutate the index (query, add, rm, get, list, index) print `No gogol daemon running. Start it with: gogol serve` if it isn't up. (`status` still works standalone as a diagnostic.)

```bash
gogol serve              # Start (backgrounds)
gogol serve --foreground # Run in foreground (debugging)
gogol serve --status     # Check if running
gogol serve --stop       # Graceful shutdown
gogol serve --tcp 0.0.0.0:9400  # Also listen on TCP (remote access)
```

### `gogol index [--index N] [--force] [--debug]`

Index files from configured directories.

```bash
gogol index              # All configured indexes
gogol index --index web  # Specific index
gogol index --force      # Re-embed everything
```

### `gogol query <text> [--index N] [--type doc|note|term] [-n K] [--show [N]] [--scores] [-v] [--path P]`

Semantic + keyword hybrid search with RRF ranking.

```bash
gogol query "auth flow"                       # Search all indexes
gogol query "payment" --index backend         # All entries in backend
gogol query "payment" --type doc              # Only file (doc) entries
gogol query "deploy" --type note              # Only memory notes
gogol query "hooks" --index web -n 10         # More results from web
gogol query "config" --show                   # Show 5 lines of content per result
gogol query "utils" --abs                     # Show absolute file paths
```

### `gogol add doc|note|term <index> <path> [content] [--stdin] [-f file] [--sources S]`

Add an entry. The type controls what is written:

| Type | `gogol add` does |
|------|-----------------|
| `note` | Creates a memory note in the index's store (embedded immediately). Works on any index regardless of mode. |
| `term` | Creates a glossary term (term → expansion) in the index's store. Works on any index. |
| `doc` | Writes a file to disk under the index's paths + embeds. Requires an `rw` index; a read-only (`r`) index rejects with `Index is read-only`. |

```bash
gogol add note web auth/session "JWT tokens in sessionStorage..."
gogol add note team deploy/notes "HPA scales on CPU at 70%"

# Read content from stdin (avoids shell escaping)
echo "complex content" | gogol add note web auth/notes --stdin

# Read content from file
gogol add note team deploy/runbook -f ~/notes/deploy.md

# Heredoc (multi-line, no escaping needed)
gogol add note team payments/flow --stdin <<'EOF'
1. Customer initiates payment
2. Backend creates a payment provider session
3. Redirect to the provider hosted page
EOF

# Glossary term
gogol add term backend OMS "order management system"
```

### `gogol rm doc|note|term <index> <path>`

Remove an entry.

```bash
gogol rm note web auth/session      # Remove memory note
gogol rm term backend OMS           # Remove glossary term
gogol rm doc web src/old.ts         # Delete file (rw index only)
```

### `gogol get doc|note|term <index> <path> [-n N]`

Retrieve content by location. `-n` limits output lines (default: full file). A `doc` path may carry a `:line` suffix.

```bash
gogol get note web auth/flows
gogol get doc web src/utils.ts:42          # full file from line 42
gogol get doc web src/utils.ts:42 -n 30    # 30 lines from line 42
gogol get doc web src/utils.ts -n 100      # first 100 lines
gogol get term backend OMS                 # glossary expansion
```

### `gogol list [doc|note|term] <index> [--path P]`

List entries. The type is optional; omitted means all types.

```bash
gogol list                            # Summary of all indexes
gogol list web                        # All entries in web (all types)
gogol list doc web                    # Only file (doc) entries in web
gogol list note web                   # Only memory notes in web
gogol list term backend               # Only glossary terms in backend
gogol list web --abs                  # Absolute file paths
gogol list web --path short           # Filename only
```

### `gogol calls <function> [--index N] [--depth D] [--in] [--out] [-v] [--path P]`

Show call graph for a function. Extracted from tree-sitter AST during indexing.

```bash
gogol calls "processOrder"                        # All indexes
gogol calls "handleEvent" --index backend        # Specific index
gogol calls "hashValue" --index checkout         # Who calls it, what it calls
gogol calls "processOrder" --depth 3              # Recursive tree (3 levels)
gogol calls "hashValue" --out                    # Only callees
gogol calls "hashValue" --in                     # Only callers
```

Output shows callers above, queried function in middle, callees below:
```
├ putOrderHashes      checkout:src/order-hash.ts:65
└ checkForAlerts      checkout:src/order-hash.ts:318

hashValue  checkout:src/order-hash.ts:36

├ digest              checkout:src/order-hash.ts:36
├ update              checkout:src/order-hash.ts:36
└ createHash          checkout:src/order-hash.ts:36
```

With `--depth 2`, shows recursive tree:
```
├ putOrderHashes      checkout:src/order-hash.ts:65
│ └ processOrderUpsertEvent checkout:src/order-hash-function.ts:97
└ checkForAlerts      checkout:src/order-hash.ts:318
  └ triggerAlerts     checkout:src/alerts.ts:52

hashValue  checkout:src/order-hash.ts:36

├ digest              checkout:src/order-hash.ts:36
├ update              checkout:src/order-hash.ts:36
└ createHash          checkout:src/order-hash.ts:36
```

### `gogol metrics [file] [--index N] [--sort c|l|p|r|d] [--limit N] [--path P]`

Show function complexity metrics. The file argument is optional — omit it to
rank functions across the whole index.

```bash
gogol metrics "orders.ts" --index backend          # all functions in the file
gogol metrics --index backend --sort c --limit 20  # top 20 by complexity (index-wide)
gogol metrics --index backend --sort d             # sort by max nesting depth
```

Sort keys (`--sort`, default `c`): `c` complexity, `l` lines, `p` params,
`r` returns, `d` max depth. `--limit`/`-n` caps the rows (default 20).

Output columns: C (cyclomatic complexity), Lines, P (params), R (returns), D (max depth).

### `gogol explore <name> [--index N] [--file P] [-n lines] [--full] [--path P]`

One-call deep dive: shows definition, source snippet, callers, callees, imports, and related symbols.

```bash
gogol explore "processOrder" --index backend       # full function context
gogol explore "checkForAlerts" --index backend     # definition + call graph + deps
gogol explore "handleEvent" -n 30                  # show 30 lines of source
gogol explore "main" --file src/cli                # disambiguate by file path
```

If a name has multiple definitions (e.g. several `main`), explore lists them and
you narrow with `--file <path-substring>` (or `--index`).

Output:
```
processOrder  backend:src/model/orders.ts:166  (106 lines)

export function processOrder(order: Order, opts?: OrderOptions): boolean {
  if (isInvalidOrder(order)) {
    return false;
  }
  const items = orderItems();
  ...

imports:
  ../components/order/order, ../components/order/orderToLineItem,
  ../navigation/navigateTo, ../navigation/Routes, ./cart, ./order

callers:
  ← retryOrder                     backend:src/model/orders.ts:279

callees:
  → isInvalidOrder                 backend:src/model/orders.ts:167
  → orderToLineItem                backend:src/model/orders.ts:171
  → thresholdChecks                backend:src/model/orders.ts:198
  ... +12 more

related:
  orderAmount                       backend:src/model/orders.ts:124
  retryOrder                      backend:src/model/orders.ts:273
```

Callees are filtered to project-defined functions only (removes stdlib noise like `.map`, `.filter`). Related section requires the daemon to be running.

### `gogol affected <files...> [--stdin] [--index N] [--depth D] [--filter glob] [--path P]`

Find all files that transitively depend on given files via the import graph. Without `--filter`, outputs all dependents.

```bash
gogol affected "order-hash.ts" --index backend                  # all dependents
gogol affected "order-hash.ts" --filter "*.test.*"              # only test files
gogol affected "orders.ts" --filter "e2e/**"                  # only e2e specs
gogol affected "utils.ts" "config.ts" --depth 3                 # multiple files, limit depth
git diff --name-only | gogol affected --stdin --index backend   # pipe from git
gogol affected "cart.ts" --abs                                  # absolute paths
```

CI integration example:
```bash
#!/usr/bin/env bash
AFFECTED=$(git diff --name-only HEAD~1 | gogol affected --stdin --index backend --filter "*.test.*")
if [ -n "$AFFECTED" ]; then
  npx vitest run $AFFECTED
fi
```

### `gogol status [--index N]`

Show index statistics.

### `gogol sync`

No-op stub. Memory notes and glossary terms live in gogol's own internal stores, so there is nothing to sync externally.

### `gogol prune`

Remove index files for indexes no longer in config.

### `gogol mcp`

Run an MCP (Model Context Protocol) server over stdio (JSON-RPC 2.0), so agents
(Claude Code, Cursor, Codex, …) call gogol operations as native tools instead of
shelling out to the CLI. Reads requests on stdin, writes one JSON response per
line to stdout.

The server is **off by default** — it refuses to start unless enabled in config:

```ini
[mcp]
enabled = true       ; default false — opt-in
tools = read         ; read (default) | read-write
```

- `enabled` — must be `true` or `gogol mcp` exits with a message.
- `tools` — `read` exposes read-only tools; `read-write` additionally exposes the
  write tools (`add_note`, `add_term`).

Exposed tools:

| Tool | Mode | Purpose |
|------|------|---------|
| `query` | read | Semantic + keyword search |
| `explore` | read | Function/doc deep-dive (source, callers, callees, related) |
| `calls` | read | Call graph for a function |
| `affected` | read | Files transitively depending on given files |
| `get` | read | Retrieve an entry by location (or by result cursor) |
| `list` | read | List entries in an index |
| `set_scope` | read | Set a session default index/type for later calls |
| `add_note` | write | Store a memory note (requires `tools = read-write`) |
| `add_term` | write | Add a glossary term (requires `tools = read-write`) |

The read tools require a running daemon (`gogol serve`); each returns a clear
error if the daemon isn't up.

## Output Format

Two modes, auto-selected:

- **Default** (terminal): leading type token + full index/path, tree chars for hierarchy, human-readable
- **Agent** (piped/LLM): leading type token + index:path:line, machine-parseable, results usable as `gogol get` input

Auto-detection: `isatty(stdout)` — agent when piped, default in terminal.  
Override: `gogol --format=agent` or `GOGOL_FORMAT=agent` env var.

### Path Display

Control path format with `--path`:

| Value | Shows | Example |
|-------|-------|---------|
| `full` (default) | index:relative:line | `backend:src/order-hash.ts:36` |
| `abs` | /absolute/path:line | `/home/user/backend/src/order-hash.ts:36` |
| `short` | filename:line | `order-hash.ts:36` |

Agent mode uses `full` paths by default (same as terminal). `--abs` is an alias for `--path abs`.

### Default (terminal)

```
doc  web src/features/auth/login.ts:10 § handleLogin
note team auth/session-flow
doc  web docs/api.md:5 § Endpoints (stale)
```

Calls output (tree structure):
```
├ putOrderHashes       backend:src/order-hash.ts:65
│ └ processOrderUpsertEvent backend:src/order-hash-function.ts:97
└ checkForAlerts       backend:src/order-hash.ts:318

hashValue  backend:src/order-hash.ts:36

├ digest               backend:src/order-hash.ts:36
├ update               backend:src/order-hash.ts:36
└ createHash           backend:src/order-hash.ts:36
```

### Agent (piped/LLM)

```
doc  web:src/features/auth/login.ts:10 handleLogin
note team:auth/session-flow
doc  web:docs/api.md:5 Endpoints~
```

Calls output in agent mode:
```
<5
<putOrderHashes checkout:src/order-hash.ts:65
<checkForAlerts checkout:src/order-hash.ts:318
>3
>digest checkout:src/order-hash.ts:36
>update checkout:src/order-hash.ts:36
>createHash checkout:src/order-hash.ts:36
```

With `--scores`: `cosine  location` (agent) or `rrf  cosine  location` (default)

Staleness: `(stale)` / `(missing)` suffix in default mode, `~` / `!` suffix in agent mode. Only shown when not ok.

## Configuration

`~/.gogol/config` (INI format):

```ini
model = ~/.gogol/models/nomic-embed-text-v1.5.Q4_K_M.gguf
precision = f16

[web]
path = ~/projects/web
ext = md,ts,tsx
mode = r

[backend]
path = ~/projects/backend
ext = md,ts
mode = r

[team]
path = ~/notes
ext = md
mode = rw

[private]
path = ~/private-knowledge
ext = md
mode = rw
```

### Fields

Per-index fields (inside a `[name]` section):

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `path` / `paths` | Yes | — | Directory path(s), comma-separated |
| `ext` | No | `md` | File extensions, comma-separated |
| `mode` | No | `r` | Access mode (`r` or `rw`) |
| `model` | No | global `model` | Override the embedding model for this index |
| `memory` | No | `~/.gogol/memory/{name}` | Override the memory-note store directory |

Global keys (before the first `[section]`):

| Field | Default | Description |
|-------|---------|-------------|
| `model` | — | Path to the embedding model (`.gguf`) |
| `precision` | `f32` | Stored embedding precision (`f32` or `f16`) |
| `tcp` | — | TCP listen address (e.g. `0.0.0.0:9400`); enables remote access |
| `batch_size` | (embedder default) | Embedding batch size |
| `watch` | `false` | Auto-reindex on filesystem changes |
| `watch_debounce_ms` | `2000` | Debounce window before a watch-triggered reindex |

### File Watching

The daemon can auto-reindex when files change. Enable in config:

```ini
watch = true
watch_debounce_ms = 2000
```

When enabled, the daemon monitors all configured index paths for filesystem changes (FSEvents on macOS, inotify on Linux). After changes settle (debounce period), triggers incremental reindex — updates embeddings, keywords, call graph, and import graph. Branch switches are handled naturally (many files change → batch reindex after settling).

### Modes

| Mode | Behavior | `add doc` | `add note` / `add term` |
|------|----------|-----------|-------------------------|
| `r` | Paths read-only | Rejected (`Index is read-only`) | Writes into gogol's store |
| `rw` | Paths read + write | Writes file + embeds | Writes into gogol's store |

`note` and `term` entries are always writable regardless of mode — they live in gogol's own internal stores, not under the index's configured paths. Only `doc` entries (files on disk) require an `rw` index.

### Chunkers

Gogol splits files into embeddable chunks using regex patterns. Built-in chunkers handle common formats:

| Extensions | Split pattern |
|-----------|---------------|
| `.md`, `.mdx` | `^#{1,2} ` (headings) |
| `.graphql`, `.gql` | `^(query\|mutation\|fragment\|type\|input\|enum\|interface\|scalar\|extend)` |
| `.yml`, `.yaml` | `^[a-zA-Z]` (top-level keys) |
| `.sql` | `^(CREATE\|ALTER\|DROP\|INSERT\|UPDATE\|DELETE\|SELECT\|-- Migration)` |
| `.ts`, `.tsx`, `.py`, `.go`, `.rs`, `.c`, `.cpp`, `.cc`, `.h`, `.hpp`, `.php` | Tree-sitter AST |

Custom chunkers override built-ins. Add a `[chunkers]` section:

```ini
[chunkers]
proto = ^(message|service|enum|rpc)\b
tf,tfvars = ^(resource|data|module|variable|output|locals)\b
dockerfile = ^(FROM|RUN|COPY|CMD|ENTRYPOINT|ENV|ARG)
```

Key is comma-separated extensions (without dots), value is a regex. Lines matching the regex start a new chunk.

### Ignore Patterns

Gogol skips `.git` by default. Additional patterns:

- `~/.gogolignore` — global
- `{scan_root}/.gogolignore` — per-project

One directory name per line. Symlinks are also skipped.

## Architecture

```
src/
├── cli/          CLI dispatch (main.cpp)
├── core/         Search pipeline, index pipeline, location parsing
├── adapters/     Protocol adapters (file, mem)
├── chunking/     File splitting (markdown, tree-sitter, window)
├── storage/      SQLite data layer (Db wrapper, SqliteBackend, migration) + legacy readers
├── embedding/    llama.cpp wrapper, provider interface
├── daemon/       Unix socket server, RPC client, wire protocol
└── config/       INI parser, file scanner, utilities
```

### Adapter Pattern

Each protocol implements the `Adapter` interface:
- **FileAdapter** — scan disk, chunk via IChunker chain, enrich embed text, stat-hash staleness
- **MemAdapter** — manage .mem store, timestamp-based staleness

### Embedding Enrichment

Each code chunk's embedding text is enriched before encoding to improve semantic search:

```
{directory_context} (also: {sibling_headings}): {path} § {heading}. {code} [{split_identifiers}]
```

Techniques applied (all zero-cost, no LLM):
- **Directory context** — last 2 meaningful path segments as module prefix
- **Sibling headings** — other function names from same file (capped at 200 chars)
- **Identifier splitting** — camelCase/snake_case split into words (`getDeliverySlot` → `get delivery slot`)
- **Import exclusion** — import statements not indexed (~35% index size reduction)

These bridge the vocabulary gap between code identifiers and natural language queries.

### Daemon

`gogol serve` starts a background process holding:
- Embedding model (GPU-accelerated) — owned by a dedicated embed thread
- All index metadata in memory (paths, chunks, lines)
- Embeddings loaded on first query per index (from the index's SQLite DB)
- Keyword search via SQLite FTS5 (queried from each index's `<name>.db`)

Architecture: single request thread (accept loop) + single embed thread (EmbedDispatcher). The embed thread owns the llama.cpp context and processes all embedding jobs sequentially. Request handlers submit jobs and either block (query, ~5ms) or fire-and-forget (add). Operations that don't need embedding (rm, get, status, list) respond instantly.

CLI commands connect via unix socket (`~/.gogol/sock`). Client sends PING on connect to verify daemon build version matches — prevents stale daemon issues after rebuilds. Daemon returns structured data (raw fields per result); client handles all output formatting locally. This means format and path mode changes never require protocol updates.

### Search Pipeline

1. Embed query with `search_query:` prefix
2. Cosine similarity against all chunks
3. SQLite FTS5 keyword search (BM25 ranking)
4. Reciprocal Rank Fusion (k=60) merges rankings
5. Per-result staleness check via adapter

### Storage

Each index is a single SQLite database at `~/.gogol/indexes/<name>.db` (WAL,
`synchronous=FULL`). One transaction persists the whole index atomically. Tables:
`entries` (metadata + embedding BLOB), `notes`, `glossary`, a unified `edges`
table (call / import / docref / type graphs), `exports`, `metrics`, and an
`entries_fts` FTS5 virtual table for keyword search.

- **Embeddings** are stored as a BLOB per entry (f16 or f32 per `precision`) and
  loaded into memory at startup for cosine search.
- **Keyword search** uses SQLite FTS5 (`entries_fts`), rebuilt in the same
  transaction as `entries` so it never drifts from the data.
- **Versioning**: `PRAGMA user_version` holds the per-index indexer schema
  version; `application_id` marks the file as a gogol DB.

SQLite is vendored as the official amalgamation (`vendor/sqlite/sqlite3.c`); the
optional `sqlite-vec` vector extension is a submodule (not yet wired in — cosine
runs in memory).

**Auto-migration**: on daemon start, any index still in the legacy per-file
layout (`.meta`/`.emb`/`.mem`/`.calls`/…) is migrated to `<name>.db`
automatically — write a temp DB, verify the round-trip (notes + glossary
content-exact), then atomically swap it in and rename the old files to
`.premigrate`. Any failure leaves the originals untouched and falls back to
reading the legacy files. The legacy readers remain solely as the migration
source (their writers are removed; the stores are read-only).

Precision: 0=f32 (4 bytes/dim), 1=f16 (2 bytes/dim). Configurable via
`precision = f16` in config (applies to the stored embedding BLOBs).

Metadata changes never require re-embedding. Embeddings only change on model or precision switch.

### Storage Layout

```
~/.gogol/
├── config
├── manifest
├── models/
│   └── nomic-embed-text-v1.5.Q4_K_M.gguf
├── .gogolignore
├── sock              (daemon socket)
├── serve.pid         (daemon PID)
├── serve.log         (daemon log)
└── indexes/
    └── {name}.db      (per-index SQLite DB: entries+embeddings, notes,
                        glossary, edges (call/import/docref/type), exports,
                        metrics, and an FTS5 keyword index — one file, one
                        atomic transaction)
```

Each index is a single SQLite database (`~/.gogol/indexes/{name}.db`). Older
per-file layouts (`.meta`/`.emb`/`.kw`/`.calls`/…) are auto-migrated to
`{name}.db` on daemon start and renamed to `.premigrate`; the legacy readers
remain only as the migration source.

## Network Access

Gogol can serve remote clients over TCP with encrypted connections.

### Server Setup

Add to `~/.gogol/config`:

```ini
tcp = 0.0.0.0:9400

[keys]
laptop = a3f2e1d4c5b6a7980123456789abcdef0123456789abcdef0123456789abcdef
agent-server = 7b1c9d4e5f6a0b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6
```

Start with TCP enabled:
```bash
gogol serve --tcp 0.0.0.0:9400
# Or just `gogol serve` if tcp is set in config
```

### Client Setup

Set environment variables:
```bash
export GOGOL_HOST=192.168.1.100:9400
export GOGOL_KEY_NAME=laptop
export GOGOL_KEY=a3f2e1d4c5b6a7980123456789abcdef0123456789abcdef0123456789abcdef
```

Then use gogol normally — all commands go to the remote daemon:
```bash
gogol query "auth flow" -n 5
gogol add note web notes "content"
gogol list web
```

### Security

- Connections are encrypted with XChaCha20-Poly1305
- Keys are 32-byte hex strings in the `[keys]` config section
- If any keys are configured, unencrypted TCP connections are rejected
- To revoke access: remove the key from config and restart daemon
- Unix socket (local) is always unencrypted (same machine)

### Generate a Key

```bash
openssl rand -hex 32
```

## Platforms

### macOS (Metal) — default

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

Metal acceleration automatic on Apple Silicon.

### Linux — CPU only

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

### Linux — NVIDIA (CUDA)

```bash
cmake -B build -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release
```

### Linux — AMD (ROCm)

```bash
cmake -B build -DGGML_HIP=ON -DCMAKE_BUILD_TYPE=Release
```

### Linux — Vulkan

```bash
cmake -B build -DGGML_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
```

## Performance

With daemon running (model pre-loaded):

| Operation | Time |
|-----------|------|
| query | ~30ms |
| add (response) | ~20ms |
| add (searchable) | ~150ms (async embed) |
| get | ~20ms |
| list | ~10ms |
| rm | ~10ms |

The daemon is required for all read/write commands (direct/standalone mode was removed). Start it once with `gogol serve`; it holds the model in memory so operations stay in the millisecond range above.
