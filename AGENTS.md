# AGENTS.md

Development guide for AI agents working on gogol.

## Project structure

```
gogol/
├── CMakeLists.txt          # Build config (llama + ggml + tree-sitter + sqlite/sqlite-vec)
├── src/
│   ├── cli/
│   │   ├── main.cpp           # CLI entry point + arg parsing
│   │   ├── commands.h         # Args struct + command declarations
│   │   ├── cmd_store.cpp      # All subcommands (recall, delve, links, metrics, etc.)
│   │   ├── git_identity.cpp/h # Git worktree/common_dir resolution
│   ├── core/
│   │   ├── refresh.cpp/h      # On-demand repo indexing (resync_disk_index, refresh_repo)
│   │   ├── result_formatters.cpp/h # Unified recall-result formatting (CLI + MCP share it)
│   │   └── types.h            # Shared types (FunctionMetrics)
│   ├── adapters/
│   │   ├── adapter.h          # AdapterChunk (the chunk type produced by a scan)
│   │   └── file_adapter.cpp/h # Disk scan + chunk + enrich (the only backend)
│   ├── chunking/
│   │   ├── chunker.cpp/h      # Window fallback chunker
│   │   ├── chunker_iface.cpp/h# IChunker interface + registry + default_index_extensions
│   │   └── ts_chunker.cpp/h   # Tree-sitter AST chunker (chunks, calls, imports, metrics)
│   ├── store/
│   │   ├── store.cpp/h        # Unified SQLite store (cache.db + notes.db)
│   ├── storage/
│   │   ├── db.cpp/h           # SQLite RAII wrapper (Db/Stmt/Tx, WAL)
│   │   └── index_file.cpp/h   # f16 <-> f32 conversion helpers
│   ├── embedding/
│   │   ├── embedder.cpp/h     # llama.cpp wrapper
│   │   ├── embed_provider.cpp/h # Warm singleton embedder (loads once per process)
│   │   └── model_prefixes.cpp/h # arch -> query/document retrieval prefix table
│   ├── mcp/
│   │   ├── mcp_server.cpp/h   # JSON-RPC 2.0 dispatch (stdio `gogol mcp`); tool handlers
│   │   ├── http_server.cpp    # `gogol serve`: POST /mcp + GET / live status dashboard
│   │   ├── message_bus.cpp/h  # agent message bus (send/receive/ack/peek/presence/listen)
│   │   ├── message_store.cpp/h# messages.db queue behind the bus (ADR-003)
│   │   ├── tool_registry.cpp/h# Tool catalog + schema (single source of truth)
│   │   └── json.h             # Minimal JSON value/serializer
│   └── config/
│       ├── config.cpp/h       # INI parser (~/.gogol/config)
│       ├── scanner.cpp/h      # Recursive file discovery + git_index_name + repo_tag
│       └── debug.h            # Debug logging macros
├── tests/
│   ├── test_all.cpp           # Unit tests (93 tests, no llama.cpp dependency)
│   └── test_ts_chunker.cpp    # Tree-sitter chunker tests (12 tests)
└── vendor/
    ├── llama.cpp/             # Git submodule (embeddings)
    ├── sqlite/ sqlite-vec/    # Vendored SQLite + vec0 ANN extension (source)
    ├── json/ CLI11/           # Vendored JSON + arg-parsing (source)
    ├── monocypher/            # BLAKE2b for content hashing (vendored source)
    ├── tree-sitter/           # Git submodule
    └── tree-sitter-*/         # Language grammars (submodules: typescript, python, go, rust, c, cpp, php, gdscript)
```

## Build

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gogol -j$(sysctl -n hw.ncpu)
```

## Development conventions

### ⚠️ NEVER touch the production database

`~/.gogol/cache.db` and `~/.gogol/notes.db` are **PRODUCTION DATA**. The notes are
LLM-authored content of record — they are NOT rebuildable from code. Deleting or
overwriting them is irreversible data loss.

**ALL development, debugging, and testing MUST happen in an isolated environment.**
No exceptions, no "just this once". Specifically:

- **Never** `rm`, `>`, or overwrite `~/.gogol/cache.db` or `~/.gogol/notes.db`.
- **Never** run `gogol index`, `gogol remember`, or any write command against the
  live store when testing a code change. Use a throwaway `GOGOL_HOME`:
  ```bash
  export GOGOL_HOME=$(mktemp -d)   # isolated cache + notes live here, not ~/.gogol
  # ... test ...
  rm -rf "$GOGOL_HOME"             # throw away the whole isolated store
  ```
  If the binary doesn't honor `GOGOL_HOME` yet, copy the DBs to a temp location
  first and point your test at the copy — never at the originals.
- **Back up BEFORE any operation that could touch prod data** — not after. A backup
  taken after a wipe is worthless. `cp ~/.gogol/notes.db ~/.gogol/notes.db.bak`
  is the first command, before anything else.
- If a reproduction needs a real index, build it in the isolated `GOGOL_HOME`, not
  by re-indexing into the live store.
- Debugging a store/query bug? Reproduce in the isolated store. Add `fprintf(stderr)`
  to the binary, rebuild, run against the copy. Never mutate prod to see what happens.

The cache.db side is derived/rebuildable (re-indexing regenerates it), but notes.db
is not. Treat both as untouchable during development.

**Safety net:** gogol auto-backs up notes.db once per calendar day into
`gogol_home()/backups/YYYY-MM-DD/`, keeping a Fibonacci-aged set {1,2,3,5,8,13}
days. This protects against the exact failure above (notes.db overwrite) for the
prod home. It is NOT a license to be careless — isolated `GOGOL_HOME` for dev is
still mandatory. The backup is a recovery mechanism, not a development workflow.

### Build & verify loop
- Build: `cmake --build build --target gogol -j$(sysctl -n hw.ncpu)`
- Reconfigure when adding source files: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
  (add the `.cpp` to BOTH the `gogol` and `gogol-tests` targets in CMakeLists.txt).
- Tests: `./build/gogol-tests` (unit) and `./build/gogol-ts-tests` (tree-sitter).
  Every change must keep both green.
- Add a unit test in `tests/test_all.cpp` for new logic — style is
  `void test_xxx() { TEST(xxx); ... assert(...); PASS(); }`, registered in `main`.

### Code style
- **RAII everywhere.** No manual resource cleanup. `Db`/`Db::Stmt`/`Db::Tx` wrap sqlite handles.
- **Non-throwing filesystem IO.** Use `std::error_code` overloads and return `false`/handle the error.
- **Bound parameters, never string-concatenated values** in SQL (`sqlite3_bind_*` / `Db::Stmt::bind`).
- **Vendor dependencies as source** in `vendor/` (compiled directly — zero external/runtime deps).

### Patterns to reuse
- **Registry** (`default_chunkers()`) — adding a new chunker is one class + one registry line.
- **Content-addressable identity** — `id = content_id(content)` (BLAKE2b hash, 16 hex chars).

## Architecture

### Daemon-less, zero-config
- No background process. The embedding model loads on first use (~2s) and stays warm.
- Auto-indexes git repos on query. No manual index commands or config sections per repo.
- HTTP transport: `gogol serve` adds a `POST /mcp` JSON-RPC endpoint and a live `GET /`
  status dashboard. Binds `127.0.0.1` by default; `--host 0.0.0.0` exposes it (NO AUTH —
  only for same-host/trusted use, e.g. containers on the docker bridge). stdio
  (`gogol mcp`) remains the default transport.
- Git identity: `idx = root commit SHA` (worktree/clone/move-stable). Worktrees share an index.

### Agent message bus (`gogol serve`)
gogol serve doubles as a broker for agent-to-agent messaging between coding-agent
sessions (host or container) over the unix socket or HTTP. See `src/mcp/message_bus.{h,cpp}`.
- **Persistent, at-least-once (ADR-003).** Queues live in `gogol_home()/messages.db`
  (`MessageStore`), opened only by the daemon under its flock. Condvars, presence and
  counters stay in RAM. Durable content still belongs in a gogol note the message points at.
- **Tools** (same registry/dispatch recipe as every other tool). Hidden from tools/list:
  `receive(me, timeout_ms?,
  consume?, session?)` (long-poll on a per-name condvar; serve is thread-per-connection),
  `ack(me, seq)`, `peek(me)`, `presence(name, online?)`. Listed only with `[mcp] messaging
  = true`: `listen(name)` and `send(to, body, me?, from?, with_context?, ref?, context_limit?)`.
- **Messaging over MCP** (`[mcp] messaging = true`, default off): for MCP-only harnesses
  (Vibe). Identity is an explicit handle, not connection state (MCP 2026-07-28 removed
  sessions, SEP-2567): `listen(name)` returns `me`; the caller (usually a client hook)
  passes `me` on later calls. `send` signs with `me` over `from`. Any tools/call carrying
  `me` gets an extra text block "📟 N unread messages for <me> (from: …)" when its inbox
  has queued messages, once per change (`MessageBus::unread_hint`). Delivery stays a
  client concern (receive with `session`, ack); the hint only says mail is waiting.
  No auth: any caller can pass any `me`; fine on one trusted host only.
- **Delivery**: `receive` without `session` consumes on receive. With `session` it claims
  the row (redelivered until `ack`) and first requeues claims of the name's other
  sessions. Daemon start requeues all claims; an hourly sweep requeues claims older than
  1h and prunes delivered rows older than 30 days. Per-inbox cap 1000 queued (drop
  oldest). seq is a persistent AUTOINCREMENT, never reused; clients dedupe by it.
- **Semantic context on send**: `send(with_context=true, ref=<root-sha>)` reuses the recall
  path (`handle_store_query`) to attach top-k hits to the message, so the recipient arrives
  pre-loaded and skips its own search turns. `ref` is a portable identity (no shared fs).
- **Observability**: `status` emits a `bus` section (agents, online, total_sent, queued,
  per-agent sent/received/inbox_depth/last_seen); the `GET /` dashboard renders it live
  (3s poll) in an Agents panel.
- **Auth/encryption**: none yet — deferred until the broker is exposed beyond a trusted
  host. Clients are thin: they call the bus tools over the socket or POST /mcp.

### Config (`~/.gogol/config`) — bare minimum
Fully optional. Everything auto-detects; a config only *overrides*. The whole file:
```ini
[global]
model = ~/.gogol/models/<model>.gguf   # optional: else GOGOL_MODEL env, else ~/.gogol/model symlink
# query_prefix / document_prefix        # optional OVERRIDE; omit to auto-select from model arch
[mcp]
enabled = true                         # start `gogol mcp` (default: off)
messaging = false                      # agent messaging over MCP: listen/send + unread hint
http_host = 127.0.0.1                   # HTTP bind for `gogol serve` (0.0.0.0 to expose; NO AUTH)
http_port = 7777                        # HTTP port for `gogol serve` (0 disables)
```
- **Retrieval prefixes auto-select** from the model's GGUF `general.architecture`
  (nomic-bert / bert / e5 — see `src/embedding/model_prefixes.cpp`); set the keys only
  for an unknown/custom model. **Context length, embedding dim, pooling** are read from
  the model. No `precision` / `tcp` / `watch` / per-`[index]` keys — those were culled
  (an old config still containing them is ignored, not errored).

### Two-DB split (cache.db + notes.db)
- **cache.db**: Derived/rebuildable data — code/doc entries, embeddings, call graph, FTS.
- **notes.db**: LLM-authored notes (content of record). Never touched by cache rebuilds.

### Store schema (cache.db)
```sql
-- idx = repo ROOT COMMIT SHA (content-derived identity); source = repo-RELATIVE path.
entries (id, content, embedding, source, idx, root_sha, line, end_line, symbol, complexity, ...)
entry_sources (entry_id, toplevel, source)  -- worktree visibility; toplevel = absolute checkout root
file_hashes (idx, source, hash)             -- incremental sync; (idx, source) key (relative source)
edges (kind, src_sym, dst_sym, idx, file)   -- call/import graph; file = relative
ref_sightings (root_sha, path, last_seen, overlay)  -- location layer: ref -> on-disk paths + overlay flag
entries_fts (content, symbol)               -- FTS5 keyword search
entries_vec (id, embedding float[768])      -- sqlite-vec vec0 ANN (cosine KNN)
```
Identity = root SHA (worktree/clone/move-stable); location = absolute paths in
`entry_sources.toplevel` + `ref_sightings.path`, reconstructed at query time. See
`docs/DESIGN-identity-and-location.md`. Transition migrations (M1–M7): removed once all
stores were migrated — `docs/MIGRATIONS.md` is now a historical record.

### Worktree support
- Same content in main + worktree = one entry row, multiple `entry_sources` rows.
- Query scopes to a toplevel via `entry_sources` join — each checkout sees only its files.

### Hybrid search
- ARM 1: Cosine similarity (semantic), range [0,1]
- ARM 2: FTS5 keyword search (BM25)
- Fused via **weighted blend** (A3): `score = 0.6·cosine + 0.4·bm25_norm`, where bm25 is
  min-max normalized within the result set. Hits present in only one arm score 0 for the
  missing arm. Chosen over plain RRF after an 8-trial known-item eval (recall@1 +19pts,
  MRR +0.10) — rank-only fusion discards the magnitude that separates near-ties when
  cosine scores cluster (observed avg top-10 spread ~0.077). Weights are tunable
  constants (`W_SEM`/`W_KW`) in `query_hybrid`.
- Call-graph expansion adds structurally-related code (marked `via_edge: true`)

## Testing

```bash
./build/gogol-tests        # 93 unit tests
./build/gogol-ts-tests     # 12 tree-sitter tests

# Integration (with model)
cd ~/projects/myrepo
gogol recall "auth"
gogol delve processOrder
gogol links handleEvent --kind call
gogol metrics --limit 10
# Repo scoping (ONE selector set everywhere — no --index): --scope <name|path>, --ref <sha>.
# Default is the CWD's repo; --scope picks another by name/path, --ref by root-commit SHA.
gogol delve processOrder --scope backend     # a symbol in another repo by name
gogol links handleEvent --ref <root-sha>      # portable SHA identity (no shared fs)
gogol metrics --scope ~/projects/other        # rank another repo by path
gogol remember "Note about auth flow"
gogol forget <id>
gogol remember "Revised note" --consolidate <id>

# message broker (gogol serve). Isolate with a throwaway GOGOL_HOME; never near prod.
gogol serve --host 127.0.0.1 --port 7777        # broker + live dashboard at http://127.0.0.1:7777/
# message tools over POST /mcp: send / receive / peek / presence (see the bus section above)
```

## Common issues

- **Model not found**: set `model` in `~/.gogol/config`, or `GOGOL_MODEL`, or the `~/.gogol/model` symlink.
- **Degenerate ranking on a non-nomic/bge/e5 model**: its arch isn't in the prefix table — set `query_prefix`/`document_prefix` in `[global]` (see the model card).
- **Abort trap on encode**: n_ubatch < n_tokens. Check embedder.cpp context params.
- **Query returns empty**: Re-index (cache.db rebuilds; no need to delete it).
  If you must reset, do it in an isolated `GOGOL_HOME` — never `rm` the prod
  `~/.gogol/cache.db` (see the prod-data rule above).
- **Linux compile errors**: Ensure `<algorithm>`, `<cstdint>` are included (GCC doesn't pull transitively).

## Known limitations

- **Deleted-file GC**: every resync ends with a purge of sources confirmed absent
  from disk (fs::exists with error_code — stat errors and out-of-root paths are
  skipped, never purged). Entries die only when their last entry_sources row is
  gone; entries_vec rows are deleted explicitly (vec0 has no cascade).
- **No cross-encoder re-ranking**: Results use the cosine+BM25 weighted blend (A3), not a
  neural cross-encoder. A cross-encoder second stage would improve precision further but
  can't run in SQLite (requires model inference); it would be an app-side stage 2.
- **ANN, not exact**: semantic search uses the sqlite-vec `vec0` KNN index
  (`entries_vec`/`notes_vec`), so cosine ranking is approximate-nearest-neighbor, not an
  exact all-pairs scan. (The old eager in-memory matrix load / brute-force cosine was
  retired; `load_matrix` is now dead code.)
