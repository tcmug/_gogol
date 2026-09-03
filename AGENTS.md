# AGENTS.md

Development guide for AI agents working on gogol.

## Project structure

```
gogol/
├── CMakeLists.txt          # Build config, links llama + ggml + tree-sitter
├── src/
│   ├── cli/
│   │   ├── main.cpp           # CLI entry point + arg parsing
│   │   ├── commands.h         # Args struct + command declarations
│   │   ├── cmd_query.cpp      # query + list commands
│   │   ├── cmd_calls.cpp      # calls + metrics + affected
│   │   ├── cmd_crud.cpp       # index, add, rm, get, status, sync, prune
│   │   └── cmd_explore.cpp    # explore (composite: source + callers + callees + imports + related)
│   ├── core/
│   │   ├── types.h            # Shared types (FunctionMetrics)
│   │   ├── version.h/cpp      # Build version (git describe, checked on daemon connect)
│   │   ├── call_graph_query.cpp/h # Shared call graph query helper (used by cmd_calls + cmd_explore)
│   │   ├── indexer.cpp/h      # Index pipeline (shared by CLI + daemon)
│   │   ├── searcher.cpp/h     # Search pipeline (shared by CLI + daemon)
│   │   ├── operations.cpp/h   # Shared add/rm/get logic (shared by CLI + daemon)
│   │   ├── format.cpp/h       # Output formatting (print_result, format_index_summary)
│   │   └── loc.cpp/h          # Location parsing + formatting
│   ├── adapters/
│   │   ├── adapter.h          # Base adapter interface
│   │   ├── file_adapter.cpp/h # file — disk scan, chunk, enrich, staleness
│   │   └── mem_adapter.cpp/h  # mem — .mem store, add/remove
│   ├── chunking/
│   │   ├── chunker.cpp/h      # Window fallback chunker
│   │   ├── chunker_iface.cpp/h# IChunker interface + registry
│   │   └── ts_chunker.cpp/h   # Tree-sitter AST chunker (chunks, calls, imports, exports, types, metrics)
│   ├── storage/
│   │   ├── db.cpp/h           # SQLite RAII wrapper (Db/Stmt/Tx, WAL, user_version) — LIVE store
│   │   ├── sqlite_backend.cpp/h # SqliteBackend: one <index>.db per index (default backend)
│   │   ├── storage_backend.h  # StorageBackend interface + open_backend() factory
│   │   ├── file_backend.cpp/h # FileBackend — READ-ONLY legacy migration source
│   │   ├── migrate.cpp/h      # auto-migrate legacy sidecars -> <index>.db on daemon start
│   │   ├── index_file.cpp/h   # Legacy split .meta + .emb readers (migration source only)
│   │   ├── migrate.cpp/h      # auto-migrate legacy sidecars -> <index>.db on daemon start
│   │   ├── call_store.cpp/h   # Call-graph edge loader (legacy TSV; read-only, migration only)
│   │   ├── import_store.cpp/h # Import graph loader (legacy TSV; read-only, migration only)
│   │   ├── docref_store.cpp/h # DocRef graph loader + resolve_doc_ref (legacy TSV; read-only)
│   │   ├── export_store.cpp/h # Exported symbols loader (legacy TSV; read-only, migration only)
│   │   ├── type_store.cpp/h   # Type hierarchy edges (TSV)
│   │   ├── metrics_store.cpp/h# Function complexity metrics (TSV)
│   │   ├── mem_store.cpp/h    # Mem entry content persistence
│   │   └── glossary_store.cpp/h # Per-index glossary persistence
│   ├── embedding/
│   │   ├── embedder.cpp/h     # llama.cpp wrapper
│   │   ├── embed_dispatcher.cpp/h # Single-thread embed queue (owns llama context)
│   │   └── embed_provider.h   # Abstract embedding interface
│   ├── daemon/
│   │   ├── embed_server.cpp/h # Daemon (holds all state, handles RPC)
│   │   ├── embed_client.cpp/h # RPC client for CLI
│   │   ├── rpc.h              # RPC command/status enums + request types
│   │   ├── rpc_v2.cpp/h       # Framed wire protocol + encryption
│   │   └── file_watcher.cpp/h # FSEvents/inotify file watching
│   ├── mcp/
│   │   ├── mcp_server.cpp/h   # MCP stdio JSON-RPC 2.0 server (gogol mcp)
│   │   ├── tool_registry.cpp/h# Tool catalog + schema (single source of truth)
│   │   └── json.h             # Minimal JSON value/serializer
│   └── config/
│       ├── config.cpp/h       # INI parser, IndexMode
│       ├── scanner.cpp/h      # Recursive file discovery
│       └── utils.h            # split_csv, count_entries
├── tests/
│   ├── test_all.cpp           # Unit tests (no llama.cpp dependency)
│   └── test_ts_chunker.cpp   # Tree-sitter chunker tests
└── vendor/
    ├── llama.cpp/             # Git submodule
    ├── monocypher/            # Encryption (XChaCha20-Poly1305)
    ├── tree-sitter/           # Git submodule
    ├── tree-sitter-cpp/       # C++ grammar
    └── tree-sitter-*/         # Language grammars (submodules)
```

## Build

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target gogol -j$(sysctl -n hw.ncpu)
```

## Development conventions

Follow these when writing C++ for gogol (an AI agent working on this repo should treat
these as the house style — they are enforced in review):

### Build & verify loop
- Build: `cmake --build build --target gogol -j$(sysctl -n hw.ncpu)`
- Reconfigure when adding source files: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
  (add the `.cpp` to BOTH the `gogol` and `gogol-tests` targets in CMakeLists.txt).
- Tests: `./build/gogol-tests` (unit, no llama dep) and `./build/gogol-ts-tests`
  (tree-sitter). Every change must keep both green.
- Add a unit test in `tests/test_all.cpp` for new logic — style is
  `void test_xxx() { TEST(xxx); ... assert(...); PASS(); }`, registered in `main`.

### Code style
- **RAII everywhere.** No manual resource cleanup. e.g. `Db`/`Db::Stmt`/`Db::Tx`
  wrap sqlite handles; a prepare always has a finalize via the wrapper's dtor.
- **Non-throwing filesystem IO.** Use the `std::error_code` overloads and return
  `false`/handle the error — do NOT let an exception escape. llama.cpp installs a
  global `std::set_terminate` → `abort()`, so any uncaught exception kills the whole
  daemon. RPC dispatch, the watcher callback, and the async index thread wrap their
  bodies in try/catch.
- **Atomic writes** for file stores: write to `path.tmp`, then `rename` (`atomic_write`).
- **Bound parameters, never string-concatenated values** in SQL (`sqlite3_bind_*` /
  `Db::Stmt::bind`). Concatenation is only acceptable for integer PRAGMA args.
- **Vendor dependencies as source** in `vendor/` (compiled directly, like
  `monocypher.c`, `sqlite3.c`, `sqlite-vec.c`) — zero external/runtime deps.

### Patterns to reuse (don't reinvent)
- **Adapter** (`Adapter`, `StorageBackend`) — swappable behind a factory; callers
  depend on the interface, never a concrete backend or on-disk format.
- **Registry** (`default_chunkers()`, `default_ref_extractors()`) — adding a new
  chunker/extractor is one class + one registry line; the pipeline iterates the list.
- **Codec / delegation** — a new backend DELEGATES to existing serialization; never
  duplicate an on-disk format. `FileBackend` load methods pass through to the free
  `load_*` functions; `SqliteBackend` reuses the keyword + f16 embedding serializers.
- **`_locked` helper split** for transactions — public `save_*` opens a `Db::Tx` and
  calls a no-tx `*_locked` helper; `save_all` opens ONE tx and calls every helper
  (SQLite has no nestable BEGIN, and the SQL lives only in the helpers — no dup).

### Refactor discipline (proven in the storage epic)
- Prefer small, behavior-preserving steps, each independently buildable + tested.
- Make a **checkpoint commit** after each safe step so there's always a return point.
- For non-trivial work, delegate implementation to sub-agents and have an
  INDEPENDENT auditor sub-agent verify (build, tests, behavior parity, no over-removal)
  before accepting — do not trust a stage's self-report.
- Capture design decisions as a doc under `docs/design/` (see
  `docs/design/storage-architecture.md`), and the feature set as notes in the `gogol`
  index (`gogol query "<topic>" --index gogol --type note`, spine at
  `note gogol features/overview`).

## Architecture

### Adapter pattern

Each protocol implements the `Adapter` interface:

```cpp
class Adapter {
  virtual string protocol() = 0;        // "file" or "mem"
  virtual vector<AdapterChunk> scan(bool force, map<string,uint64_t>& hashes) = 0;
  virtual EntryStatus check_stale(const string& key, uint64_t hash) = 0;
  virtual string get_content(const string& key, uint32_t line, int max_lines) = 0;
  virtual bool add(const string& key, const string& content, const vector<string>& sources);
  virtual bool remove(const string& key);
};
```

- **FileAdapter** — scans disk, chunks via IChunker chain, enriches embed text, stat-hash staleness
- **MemAdapter** — manages .mem store, timestamp-based staleness

### Shared operations (core/operations.cpp)

Type dispatch is centralized in three functions:
- `op_add(type, index, path, content, sources, config, embedder, index)` — handles doc, note, term
- `op_rm(type, index, path, config, index)` — handles doc, note, term
- `op_get(type, index, path, config)` — handles doc, note, term

Both CLI (cmd_crud.cpp) and daemon (embed_server.cpp) call these. Adding a new entry type only requires changes to `operations.cpp` + `loc.cpp`.

### Chunker interface

```cpp
class IChunker {
  virtual bool supports(const string& ext) = 0;
  virtual vector<Chunk> chunk(const string& content, const string& path) = 0;
};
```

Chain order (first match wins):
1. User-defined regex chunkers from `[chunkers]` config section
2. Built-in RegexChunker: Markdown (`^#{1,2} ` headings)
3. TreeSitterChunker: TS, TSX, Python, Go, Rust, C, C++, PHP
4. Built-in RegexChunker: GraphQL, YAML, SQL
5. WindowChunker: fixed 2000-char window (universal fallback)

### Embedding enrichment

Each code chunk's embedding text is enriched via `enrich_embed_text()` in file_adapter.cpp:

```
{directory_context} (also: {sibling_headings}): {path} § {heading}. {code} [{split_identifiers}] {glossary_expansions}
```

Techniques (all zero-cost, no LLM):
- **Directory context** — last 2 meaningful path segments
- **Sibling headings** — other chunk headings from same file, capped at 200 chars
- **Identifier splitting** — camelCase/snake_case → words
- **Glossary expansion** — matched glossary terms for the index appended
- **Import exclusion** — import statements not indexed (~35% size reduction)

### Daemon (gogol serve)

Holds all state in memory:
- Embedding model (GPU-accelerated) — owned by EmbedDispatcher worker thread
- All indexes + keyword indexes
- Accepts RPC commands via unix socket + optional TCP
- Optional file watcher: auto-reindex on filesystem changes (FSEvents/inotify)

Architecture: thread-per-connection (accept loop spawns a handler thread per client, tracked in a registry) + single writer thread (WriteQueue) + single embed thread (dispatcher). The embed thread owns the llama.cpp context (not thread-safe). Handlers read immutable snapshots lock-free and submit writes to the queue, so concurrent clients are served simultaneously; a slow query or a `submit_wait` (rm/glossary) on one connection no longer stalls others. rm/get/status/list never block on embedding.

Exception safety: llama.cpp installs a global `std::set_terminate` handler that calls `abort()`, so any uncaught exception anywhere in the process kills the whole daemon. To contain this, the RPC dispatch, the file-watcher reindex callback, and the async index thread each wrap their body in try/catch and convert failures to an error response / log line. Filesystem helpers (`atomic_write`, `create_directories` in adapters and stores) use the `error_code` overloads and return `false` instead of throwing.

Concurrency (immutable snapshots): `DaemonState` holds each index as a `shared_ptr<const Index>` (and kw index as `shared_ptr<const KeywordIndex>`). Readers (query/list/status) grab a stable snapshot under a brief lock, then read it lock-free — the snapshot stays alive via refcount even if a writer swaps concurrently. Writers (index/add/rm/watcher) deep-copy the current snapshot (`mutable_copy`), mutate the copy, then atomically publish it (`publish`). No lock is held during the expensive reindex/search work. This makes the data-race SIGSEGV class (a reader touching a half-swapped `entries`/`embedding` vector) unrepresentable rather than merely avoided at known sites. Embeddings are materialized (`ensure_embeddings`) on the mutable copy before publishing, so a `const` snapshot is never lazily mutated. There is no coarse per-request lock, so a slow query no longer blocks other clients, and RM no longer self-deadlocks.

Single writer (`WriteQueue`, `daemon/write_queue.h`): all in-memory index mutations run on one owner thread. INDEX/ADD/RM/watcher submit jobs; each job does the `mutable_copy → mutate → publish` sequence. Because only the writer thread mutates, the snapshot a job copies is always the latest published one — there is no lost-update window between concurrent writers. `submit()` is fire-and-forget (async index, mem-add publish); `submit_wait<T>()` blocks for a result (rm, glossary add) so the client still gets a synchronous ack. The `.mem` and glossary *store* writes also run on the writer thread now (mem-add writes `.mem` inside the same job that publishes the index; glossary/rm run under `submit_wait`), so the stores are single-owner and can't lose updates. Embedding still routes through `EmbedDispatcher` (sole llama-context owner); the writer thread blocks on those embeds without deadlock (the dispatcher serializes writer and query embeds on its own thread).

Thread-per-connection: the accept loop spawns a handler thread per client (tracked in a `HandlerRegistry`), so a handler blocking on `submit_wait` (rm/glossary) or a slow query no longer prevents `accept()` or stalls other clients. This is safe because reads use immutable snapshots and all writes are serialized on the single writer thread — handler threads never mutate shared state directly. `indexing_name` (read by STATUS on a handler thread, written by the writer thread) is guarded by a small mutex; other status fields are atomics. Finished threads are reaped (joined) each accept tick to bound registry growth. On shutdown the daemon stops accepting, calls `shutdown(SHUT_RDWR)` on each still-running client fd to unblock any handler stuck in `read()`, then joins all handler threads before destroying state — no detached threads, no use-after-free, no bounded-timeout guesswork. The listen backlog is 128 (was 8) to absorb connection bursts.

Version check: client sends PING on connect, daemon returns build version. If mismatch, the client prints restart instructions and exits (there is no direct-mode fallback).

CLI commands connect to the daemon over the unix socket. Direct/standalone mode was removed: read/write commands (query, add, rm, get, list, index) require a running daemon and print `No gogol daemon running. Start it with: gogol serve` if it isn't up. `status` still works standalone (reads index-file headers) as a diagnostic. The call-graph read commands (calls, metrics, affected, explore) read the auxiliary TSV stores directly since they have no daemon RPC and never mutate.

Network: `gogol serve --tcp 0.0.0.0:9400` enables TCP. Encrypted with XChaCha20-Poly1305 when `[keys]` section exists in config. Client uses `GOGOL_HOST`, `GOGOL_KEY_NAME`, `GOGOL_KEY` env vars.

File watching: `watch = true` in config. Daemon monitors indexed paths, debounces changes, triggers incremental reindex. Branch switches handled naturally.

RPC commands: PING (returns build version), SHUTDOWN, QUERY, ADD, RM, GET, LIST, INDEX, STATUS.

Daemon returns structured data (raw fields per result). Client handles all output formatting locally — path modes, format selection, tree rendering. This means format changes never require protocol changes.

### MCP server (gogol mcp)

`gogol mcp` runs a Model Context Protocol server over stdio (JSON-RPC 2.0), so
agents call gogol operations as native tools. Architecture:

- **`mcp/tool_registry.cpp`** is the single source of truth: each tool is one
  `ToolDef` (name, description, param schema, `read_only`). The tools/list JSON
  Schema, tools/call validation, and dispatch all derive from it — adding a tool
  touches only its `ToolDef`.
- **`mcp/mcp_server.cpp`** is protocol glue (initialize / tools/list / tools/call)
  and reuses the existing search/call-graph logic; read tools connect to the
  daemon over the unix socket exactly like the CLI.
- **Config-gated**: off unless `[mcp] enabled = true`. `tools = read` (default)
  exposes read-only tools; `tools = read-write` additionally exposes the write
  tools. Only `enabled` and `tools` are parsed today.
- Read-only tools: `query`, `explore`, `calls`, `affected`, `get`, `list`,
  `set_scope`. Write tools (gated): `add_note`, `add_term`.
- Sessions are per-connection (default index/type scope, result cursors); state
  lives in the `gogol mcp` process, not the daemon.

### Entry types

Every entry is addressed as `<type> <index> <path>`:

- `doc web src/utils.ts:42 § heading` — file chunk (under the index's configured paths)
- `note web auth/flows` — memory note (in the index's store)
- `term web OMS` — glossary term (term → expansion)

`add`, `get`, and `rm` take the type as the first argument. `list` and `query` take an optional type (positional for `list`, `--type` for `query`); omitted means all types.

### Index format

Each index is a single SQLite database at `~/.gogol/indexes/<name>.db` (WAL,
`synchronous=FULL`, `application_id` = gogol magic, `user_version` = indexer
schema version). One transaction persists the whole index. Tables: `entries`
(metadata + embedding BLOB), `notes`, `glossary`, a unified `edges` table
(call/import/docref/type), `exports`, `metrics`, and an `entries_fts` FTS5
virtual table for keyword search. See `docs/design/storage-architecture.md` for
the full schema.

- **Embeddings** are a per-entry BLOB (f16 or f32 per `precision`), loaded into
  memory at startup for cosine search.
- **Precision**: 0=f32 (4 bytes/dim), 1=f16 (2 bytes/dim). Set via
  `precision = f16` in config.

**Legacy layout (migration source only).** Older indexes used split
per-file stores with independent versioning — `.meta` (`magic "GOMT"`, entries)
and `.emb` (`magic "GOEB"`, embeddings), plus `.kw`/`.calls`/`.imports`/… TSVs.
On daemon start these are auto-migrated to `<name>.db` and renamed `.premigrate`;
the legacy readers survive only as the migration source (their writers are
removed). Protocol byte in the old `.meta`: 0=FILE, 1=MEM.

### Type filter

Query and list can restrict results to a single entry type:

```bash
gogol query "auth" --index web              # all entries in web (all types)
gogol query "auth" --type doc --index web   # only file (doc) entries
gogol query "deploy" --type note            # only memory notes
gogol query "term" --type term              # only glossary terms
gogol list note web                         # only memory notes in web
```

`query` takes `--type doc|note|term`; `list` takes an optional positional type (`gogol list <type> <index>`). Omitted means all types.

## Configuration

`~/.gogol/config`:

```ini
model = ~/models/nomic-embed-text-v1.5.Q8_0.gguf

[web]
path = ~/projects/web
ext = md,ts,tsx
mode = r
```

### Modes

| Mode | `add doc` | `add note` / `add term` |
|------|-----------|-------------------------|
| `r` | Rejected (`Index is read-only`) | Writes into gogol's store |
| `rw` | Write file + embed immediately | Writes into gogol's store |

`note` and `term` entries are always writable regardless of mode (they live in gogol's own internal stores). Only `doc` entries require an `rw` index.

## Adding a new entry type

1. Update `loc.cpp` — parsing logic for the new type
2. Update `loc.h` — add field to `ParsedLoc`, add format function
3. Update `operations.cpp` — add case in `op_add`, `op_rm`, `op_get`
4. Update LIST handler in `cmd_query.cpp` and `embed_server.cpp`
5. Add storage backend if needed (like `glossary_store.cpp`)

## Adding a new subcommand

1. Add handler in the appropriate `src/cli/cmd_*.cpp` file
2. Register CLI option in `src/cli/main.cpp`
3. Add daemon RPC handler if needed (`src/daemon/embed_server.cpp`)
4. Add client method if needed (`src/daemon/embed_client.cpp`)

## Adding a new chunker

1. Create class implementing `IChunker` in `src/chunking/`
2. Add to `default_chunkers()` in `chunker_iface.cpp` (order matters — first match wins)

Or add a regex pattern in `[chunkers]` config section (no code change needed).

## Adding a new tree-sitter grammar

1. `git submodule add` the grammar repo into `vendor/`
2. Add its `parser.c` to `ts-grammars` in CMakeLists.txt
3. Add `extern "C"` declaration in `ts_chunker.cpp`
4. Add extension mapping in `get_language()`

## Testing

```bash
./build/gogol-tests        # unit tests (62 tests)
./build/gogol-ts-tests     # tree-sitter tests (10 tests)

# Integration (requires model + daemon)
gogol serve
gogol index --index web
gogol query "auth" -n 3
gogol query "auth" --path short              # filename:line format
gogol query "auth" --path abs                # absolute paths
gogol query "auth" --index web               # scope to web index
gogol query "auth" --type doc --index web    # only file (doc) entries
gogol query "deploy" --type note             # only memory notes
gogol add note private test "content"
echo "multi-line" | gogol add note private test2 --stdin
gogol add note private test3 -f ~/notes.md
gogol get note private test
gogol get note private test -n 5             # limit lines
gogol rm note private test
gogol add term private API "application programming interface"
gogol get term private API
gogol rm term private API
gogol calls "processOrder" --index backend
gogol calls "hashValue" --index backend --depth 2
gogol calls "hashValue" --index backend --out   # callees only
gogol calls "hashValue" --index backend --in    # callers only
gogol metrics "orders.ts" --index backend
gogol metrics "orders.ts" --index backend --path abs
gogol explore "processOrder" --index backend
gogol explore "checkForAlerts" --index backend -n 30
gogol affected "order-hash.ts" --index backend
gogol affected "order-hash.ts" --index backend --filter "*.test.*"
echo "order-hash.ts" | gogol affected --stdin --index backend
gogol list web
gogol list web --path short
gogol status
gogol serve --stop
```

## Common issues

- **Model not found**: Set `model` in `~/.gogol/config` (before first `[section]`)
- **Abort trap on encode**: n_ubatch < n_tokens. Check embedder.cpp context params.
- **Old index not loading**: Schema/format mismatch. Re-index with `gogol index --index <name> --force` (or delete `~/.gogol/indexes/<name>.db` and re-index).
- **No files found**: Check `~/.gogolignore` patterns. Only `.git` is skipped by default.
- **Daemon won't start**: Check `~/.gogol/serve.log`. Remove stale `~/.gogol/sock` if exists.
- **"Daemon already running" but it's not**: Remove `~/.gogol/sock` and `~/.gogol/serve.pid`.
- **Query returns empty**: Check daemon version matches binary (`gogol serve --stop && gogol serve`).
- **Linux: daemon exits immediately**: Stale socket from previous crash. Remove `~/.gogol/sock`.
- **Linux: compile errors**: Ensure `<algorithm>`, `<cstdint>` are included (GCC doesn't pull transitively).

## Known limitations & future improvements

### Architecture
- **Handler-thread cancellation granularity**: connection handlers are tracked in a registry and joined on shutdown; `shutdown(SHUT_RDWR)` on each still-running client fd unblocks a stuck `read()` so joins complete. Cancellation is fd-level (close the socket), not cooperative — a handler wedged in CPU work rather than a socket read would still delay shutdown until it returns. Fine in practice since handlers are short (snapshot read or a queue submit).
- **Daemon auto-restart on version mismatch**: Currently the client prints restart instructions and exits. Should auto-kill + restart the daemon transparently. Related: when no daemon is running, read/write commands just fail with a message — they could auto-start a daemon (and have it self-exit after an idle period).

### Index format
- **Orphaned embeddings after rm (RETIRED)**: this only ever affected the legacy file layout, where `save_index_meta_only` rewrote `.meta` and left the removed entry's `.emb` slot dead until the next full re-index. Runtime writes now go through `SqliteBackend` (rm deletes the row — no orphans), so the concern is dead. `FileBackend` is read-only (its savers throw); the free `save_index_meta_only` in `index_file.cpp` is dead on the runtime path (kept only for legacy `.meta`/`.emb` read-path test coverage).
- **No schema version per index**: When extraction changes (new fields in .imports, .types, etc.), indexes need manual `--force` re-index. Should auto-detect and re-index on daemon startup.
- **Embeddings loaded eagerly**: `ensure_embeddings()` loads every entry's embedding BLOB from `<name>.db` into memory at startup for cosine search (tens of MB per large index). Could memory-map or use `sqlite-vec` for on-disk vector scan instead.

### Search quality
- **No re-ranking**: Results are pure cosine + BM25 RRF. A cross-encoder re-ranker on top-20 could improve precision significantly.
- **No query expansion**: Short queries like "TTL" don't benefit from semantic search. Could expand via glossary entries automatically.
- **Embedding staleness**: Search returns stale entries (with warning). Could filter them by default and show with `--stale` flag.

### Daemon
- **No graceful embed queue drain on shutdown**: `EmbedDispatcher` destructor waits for current job but drops queued ones. Pending adds lose their embeddings (content is in .mem, just not searchable until next index).

### CLI
- **No completion**: Tab completion for index names, scope patterns, and commands would reduce friction.
- **No `gogol compact`**: No explicit command to `VACUUM` an index DB / reclaim space after large removals (SQLite reuses freed pages, but the file doesn't shrink).
