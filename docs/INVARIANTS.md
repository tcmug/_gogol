# gogol store invariants

Hard-won contracts that are enforced at *every* call site but are easy to miss at
*one*. Each invariant below has a history of production bugs behind it (cited by
commit). Violating one typically produces **silent-wrong results or UB**, not an
error — which is why they must be documented rather than rediscovered.

These complement `AGENTS.md` (schema overview) and `docs/DESIGN.md`. When you touch
`store.cpp`, `refresh.cpp`, the chunker, or the embedder, re-read the relevant
section.

---

## 1. Content-addressable identity → satellite reconciliation

`id = content_id(content)` (BLAKE2b of the content alone — no salt). This is the
dedup invariant: identical content is one row, accumulating each source's index
tag.

**The consequence that bites:** when a file changes, its chunks get **new ids**.
The old rows do **not** update in place — they orphan. Every table that references
an entry id is a *satellite* that must be reconciled when content changes or a file
is deleted. Reconcile at every mutation site, not just the one you're editing.

### Satellite inventory (cache.db)

| Table / column | Keyed by | Reconciliation on change/delete | Mechanism |
|----------------|----------|---------------------------------|-----------|
| `entries` | `id` | the row itself | `DELETE FROM entries` |
| `entries_vec` (vec0) | `id` | **explicit DELETE** — vec0 has **no** `ON DELETE CASCADE** | hand-written `DELETE FROM entries_vec` in every delete path |
| `entries_fts` (fts5) | `rowid` | automatic via `entries_ai/ad/au` triggers | triggers, do not hand-delete |
| `entry_sources` | `entry_id` | automatic via `FOREIGN KEY ... ON DELETE CASCADE` | cascade (needs `PRAGMA foreign_keys=ON`) |
| `edges` | `file` (not id) | **explicit DELETE by file** on deleted files | `DELETE FROM edges WHERE file=?` |
| `file_hashes` | `source` | **explicit DELETE by source** on deleted files | `DELETE FROM file_hashes WHERE source=?` |
| `entries.symbol` | column on entries | goes with the row (no side table) | inherent — symbols were collapsed into entries |

### The rule

- **vec0 tables (`entries_vec`, `notes_vec`) have no cascade.** Every path that
  deletes an entry/note MUST hand-delete the matching vec row. Miss it and you leak
  a dead 768-dim vector that every KNN scan still touches (correctness *and* perf).
  Cited: `28e1b25` (stale purge leaked vec rows), `764a70d` (orphaned refs rendered
  as garbage bytes via unguarded `get()`).
- **`entries_fts` is trigger-maintained** — never hand-delete; let the triggers
  fire so it can't drift.
- **`entry_sources` relies on `ON DELETE CASCADE`** — the foreign key only fires if
  `PRAGMA foreign_keys=ON` is set on the connection.
- **`edges` is file-keyed, not id-keyed.** A deleted file's call/import edges dangle
  unless dropped by `file`. Cited: `1e8312a` (dangling edges on deleted files).

### The two delete surfaces (keep them symmetric)

- `remove_by_source(source)` — a **deleted** file: drop entries + `entries_vec` +
  `edges` (by file) + `file_hashes` (by source).
- `remove_stale_chunks(source, keep_ids)` — a **changed** file: drop the entries
  (and `entries_vec`) **not** in the current chunk-id set, keeping unchanged chunks
  so their `created` stamp survives. Cited: `94cc371` (changed files left stale
  old-content chunks; recall served code that no longer existed).

If you add a new table referencing entry id, add it to **both** surfaces and to the
table above. The recurring historical bug ("same class as the orphaned symbol-map")
was always a satellite reconciled at one site but not the next.

### Re-put must backfill, not no-op

`put()` upserts with `ON CONFLICT(id) DO UPDATE ... COALESCE(excluded, old)`. A plain
`INSERT OR IGNORE` would no-op on content-id dedup, so a re-sync would never populate
columns added after a row was first written. Cited: `3564352` (40,155 rows, 0
populated, because `INSERT OR IGNORE` never backfilled new columns).

---

## 2. notes.db is a separate world (no entry_sources, no graph)

Notes live in `notes.db`, not `cache.db`. A note row is: content-hash `id`,
`content`, `embedding` (co-located so a cache rebuild leaves notes queryable),
optional `scope_path`, `created`. **No `entry_sources`, no `edges`, no FTS.**

Consequences that bite:

- **Notes are scoped by `scope_path` (path-prefix), NOT by toplevel visibility.**
  The `entry_sources` toplevel-visibility join that scopes code/doc results would
  drop every note (notes have no `entry_sources` row). Recall must *bypass* that join
  for note ids. Cited: `444ad8b` (scoped notes vanished whenever any `--index`/
  `--scope` was set).
- **`notes_vec` has the same no-cascade rule as `entries_vec`** — `forget_note` and
  `consolidate_note` hand-delete it.
- **`forget_note` refuses cache.db ids structurally** — notes.db is a different
  file, so a code/doc id simply isn't present to delete. The "refuses non-note" is
  structural, not a guard.

---

## 3. Search filters must be PRE-filters (cosine is scored outside SQL)

Cosine ranking runs as a vec0 KNN plus in-memory blend — it is **not** a SQL `WHERE`
you can ride along. Therefore any filter that must *shrink the candidate set* has to
run as a **pre-filter** on candidate ids, never as a post-slice of the global top-k.

- `--type note|doc|code` is a **pre**-filter. Applied as a post-filter it returns
  nothing: ~300 notes never beat ~68k code/doc entries into the global top-k, so the
  post-slice is empty. Cited: `32a35e2` (type filter was broken for exactly this
  reason).
- Index/scope restriction is likewise intersected into the candidate set before
  scoring.

If you add a new filter dimension, thread it into the candidate pre-filter in
`query()`/`query_hybrid`, not after the top-k.

---

## 4. Index-name + ignore normalization: one normalizer, all sites

- **The index name (`idx`) must be normalized identically by every producer and
  consumer.** The historical `.git`-suffix bug (`50cda14`) and the bare-name
  resolution gap (`d87e3b8`, which was wired into `recall` but not
  `delve`/`links`/`literal`) are the same class: a normalization applied at some
  sites and not others → silent empty results. When you add an index-scoped surface,
  route it through the shared resolver (`resolve_scope` / `resolve_recall_scope`).
- **"Ignored" == "deleted" == "not in the index."** Sync's stale check must treat a
  newly-ignored (but still-on-disk) file the same as a removed file, or its chunks
  survive forever. Cited: `112b78a` (27,780 stale entries from newly-ignored repos).
  Reuse the scanner's `path_is_ignored` — never a bespoke skip list (`ea18f2e`).

---

## 5. Embedding-model contract (llama.cpp / ggml)

The embedder must be **model-agnostic**, and llama/ggml violate constraints by
**asserting or segfaulting**, not by returning an error code. Assume nothing.

- **Non-causal models need the whole sequence in ONE micro-batch** (no chunked
  prefill). Cap tokens at `min(n_ctx, n_ubatch)` — truncating to `n_ctx` alone is
  not enough because llama rounds `n_ubatch` up. Cited: `85b7b80`.
- **Derive sizes from the model** (`llama_model_n_ctx_train`, clamped), never
  hardcode. A hardcoded `n_ctx=2048` aborted small-context models. Cited: `1dd0fff`.
- **Batch sequence count must not exceed `n_seq_max`** or `llama_encode` segfaults
  internally. Cited: `bfaa1b1`.
- **ggml backend init writes device/Metal lines directly to stdout**, bypassing
  `llama_log_set`. On the MCP JSON-RPC-over-stdout transport this corrupts the
  stream and the client drops the connection. Guard fd 1 → fd 2 around embedder
  acquisition and embed calls. Cited: `d284de7`.

---

## 6. Process teardown: short-lived tools must `_Exit`

The vendored llama/ggml **Metal backend aborts at static-destructor time**
(`__cxa_finalize`) if resources are outstanding (`ggml_metal_device_free` asserts 0
outstanding resource sets). A short-lived stdio tool (`gogol mcp`) should flush
stdout and `std::_Exit(rc)`, bypassing C++ static-destructor teardown entirely — the
OS reclaims everything. Any background thread holding a Metal context must be joined
before exit, not detached. Cited: `d29ae22`, `7fb9d38` (Metal state is also not
fork-safe, which is why the daemon was retired).

---

## 7. Reads open read-only (never lock during a sync)

Read actions (`status`, `recall`, `delve`, `links`, `metrics`, `literal`) open the
store **read-only** (`SQLITE_OPEN_READONLY` + `query_only`, no write pragmas, no
schema DDL). A reader that opens read-write contends for the writer's lock during a
sync and fails with "database is locked". Cited: `b8599aa`. Corollary: a reader must
not run DDL — migration is the writer's job (`bc24b62` handles the first-upgrade case
with a one-time RW migration probe, then reopens pure read-only).
