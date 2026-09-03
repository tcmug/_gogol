# gogol Storage Architecture — Design Proposal

Status: DRAFT for review. Researched 2026-08-30. No code written yet.

## Problem

gogol currently persists each index as a **pile-of-files**: 11+ separate files
(`.meta .emb .kw .mem .calls .imports .exports .types .metrics .docrefs .glossary`).
This has three concrete problems:

1. **Precious data has no safety net.** Notes (`.mem`) and glossary are the *sole
   copy* of user-authored data. A custom binary format + no transactions + no
   backup = a single bad write or bit-flip can lose everything. `atomic_write`
   (write-tmp-then-rename) protects against torn writes but does **not** `fsync`,
   so a power-loss durability gap remains, and corruption after the rename point
   is silently truncated on load.
2. **No unification / hard to evolve.** Each store hand-rolls its own
   serialization and versioning (or lacks versioning entirely — the TSV sidecars
   have no version at all).
3. **"Get it right once."** We want a single abstraction where the on-disk
   encoding and the *backend* (files / SQLite / remote SQL) are swappable, with
   optional compression and encryption, without rippling changes into the rest
   of the code.

## Research summary (what mature tools do)

- **Two-tier split is universal.** sqlite-vss, hnsqlite, LanceDB, Faiss all
  separate the *vector blob* (optimized for random access / mmap / SIMD scan)
  from the *metadata/relational* layer. Nobody stores 35MB of contiguous floats
  in a row-store the same way as small records.
- **SQLite is the canonical "application file format"** for the metadata/precious
  tier. SQLite's own whitepaper argues exactly gogol's case, explicitly against
  "pile-of-files": atomic transactions, single-file backup, incremental page-level
  updates, a 4-byte Application ID header (same idea as gogol's magic bytes),
  `PRAGMA user_version` (the versioning we need, for free), and small-BLOB reads
  *faster than the filesystem*. Recommended by the US Library of Congress for
  long-term preservation.
- **Durability recipe:** WAL mode + `synchronous=FULL` for a single-writer local
  DB (matches gogol's single-writer daemon). The UMass RocksDB-vs-SQLite study
  found SQLite(FULL) the better-balanced durable choice for small-memory,
  single-writer workloads.
- **Embeddings want a dedicated layout.** LanceDB (up to ~1000× faster random
  access than Parquet) and Faiss show the vector tier benefits from a
  purpose-built format. gogol's flat-float mmap `.emb` is already the right
  instinct for a local tool — keep it.

## Data inventory (ground truth from src/storage)

| Data       | Struct              | Tier       | Durability need |
|------------|---------------------|------------|-----------------|
| entries    | `IndexEntry`        | metadata   | rebuildable     |
| embeddings | `float[dim]×N`      | **vector** | rebuildable (expensive) |
| keyword    | BM25 postings       | derived    | rebuildable     |
| **notes**  | `MemEntry`          | **precious** | **sole copy** |
| **glossary** | `map<term,exp>`   | **precious** | **sole copy** |
| calls      | `StoredCallEdge`    | derived    | rebuildable     |
| imports    | `ImportInfo`        | derived    | rebuildable     |
| docrefs    | `DocRefEdge`        | derived    | rebuildable     |
| exports    | `map<file,syms>`    | derived    | rebuildable     |
| types      | `StoredTypeEdge`    | derived    | rebuildable     |
| metrics    | `FunctionMetrics`   | derived    | rebuildable     |

Two data classes drive the design:
- **Derived** — reproducible from source. Loss = recompute.
- **Precious** — user-authored, unrecoverable. Loss = catastrophe. (notes, glossary)

## Scope decision (2026-08-30): no embedding special-case

Measured: total embeddings across ALL indexes = **61 MB** (largest single index
= backend, 36 MB ≈ 24k × 768 × f16). This is small. The daemon already loads all
embeddings into RAM at startup and runs cosine over an in-memory flat array.

Consequence: **the on-disk format of embeddings is irrelevant to query speed** —
they are read once at load, then scanned in memory. So there is NO need for a
special mmap/columnar vector store. Embeddings become just another collection
through the same interface. This removes the only thing that forced a two-tier
design. Priorities are now: future-proofing, clean architecture, clean code, and
performance **where it matters (the in-memory hot path, not the disk format)**.

## Proposed architecture — one uniform, layered, pluggable layer

```
Domain structs (IndexEntry, Embedding, MemEntry, StoredCallEdge, DocRefEdge, ...) ← unchanged
        │
Store<T> interface:  load() -> vector<T>;  save(vector<T>)  (atomic + durable)
        │
Codec<T> (struct <-> bytes)   ── decorators: Compress(zstd), Encrypt(XChaCha20)
        │
StorageBackend (where bytes live)  ── factory, selected per index from config
   ├ FileBackend    (current multi-file; add fsync; TSV codec for precious)
   ├ SqliteBackend  (one <index>.db, ALL collections incl. embeddings; WAL,
   │                 synchronous=FULL, user_version)
   └ SqlBackend     (mysql/mariadb: shared/team indexes)     [future]
```

No `EmbeddingStore` special-case. Embeddings are a `Store<Embedding>` like any
other collection — loaded into RAM once at startup; the cosine hot path operates
on the in-memory array and is untouched by the storage layer.

- **Performance separation:** disk layer = correctness/durability/simplicity;
  hot path = in-memory cosine + BM25 (unchanged). The refactor MUST NOT touch the
  search inner loop — it only changes how bytes move in/out of in-memory structs.
- **Compression:** `CompressingCodec<T>` decorator (zstd) — opt-in per store.
- **Encryption:** `EncryptingCodec<T>` decorator reusing vendored monocypher
  (XChaCha20-Poly1305, already used for TCP) — opt-in for precious/private stores.
- **Backend selection:** per-index config, e.g. `backend = file|sqlite|mysql://...`.

## Recommendation

**Default = SqliteBackend for the whole index** — one `<index>.db`, every
collection (entries, embeddings, notes, glossary, calls, imports, docrefs,
exports, types, metrics) a table. This:
- kills the precious-data-loss risk (notes/glossary become ACID rows, single-file
  backup via copy or `.dump`),
- unifies 11 files into one `<index>.db`,
- gives `user_version`-based indexer versioning natively,
- pays SQLite's per-row overhead once at load (not per query) — safe because the
  hot path is in-memory,
- keeps `file` and `mysql` backends selectable via the same interface.

## DECISION: SQLite is the store (2026-08-30)

Confirmed technology choice. This is a well-proven pattern: SQLite + a vector
extension + FTS5 + RRF fusion is a common recipe for exactly this local
semantic-search-plus-keyword workload. We follow it.

SQLite is adopted **behind a `StorageBackend` adapter** (see next section) — it is
the first and default implementation, NOT hardcoded. The whole point is that the
backend is swappable and a remote DB is a config change, not a rewrite.

- **SQLite** vendored as the `sqlite3.c` amalgamation in `vendor/` (compiled
  directly, same as `vendor/monocypher/monocypher.c` today → zero external dep).
- **Vectors:** `sqlite-vec` (asg017) — single C file, statically compiled into the
  binary, `vec0` virtual tables. Chosen over `sqliteai-vector` (packaged as a
  Python wheel) because gogol is C++ and wants static linking, no runtime
  `load_extension` — the C-native variant fits a single self-contained binary.
- **Keyword:** FTS5 (built into the SQLite amalgamation, enable with
  `-DSQLITE_ENABLE_FTS5`).
- **Vector search is OPTIONAL at current scale.** 61 MB total, ~24k vectors/index,
  in-memory cosine is sub-ms. Phase 1 keeps the in-memory cosine (embeddings stored
  as a BLOB column, loaded once at startup). `sqlite-vec` scan/quantization is a
  later opt-in for scale — not required to ship.

## The swappable backend adapter (REQUIRED)

The daemon, indexer, and CLI depend ONLY on an abstract `StorageBackend` interface —
never on SQLite (or files) directly. Backends are interchangeable and chosen per
index from config. This makes a remote DB a future drop-in.

```cpp
// One handle per index. Loads all collections into the in-memory structs the
// daemon already uses, and persists them. Implementations decide the medium.
class StorageBackend {
public:
  virtual ~StorageBackend() = default;

  // Bulk load/save (single transaction where the backend supports it).
  virtual IndexData      load_all() = 0;          // entries+emb+notes+glossary+edges+...
  virtual void           save_all(const IndexData&) = 0;

  // Fine-grained mutations (used by add/rm without a full rewrite).
  virtual void           put_note(const MemEntry&) = 0;
  virtual void           del_note(const std::string& topic) = 0;
  virtual void           put_glossary(const std::string& term, const std::string& exp) = 0;
  // ... edges, etc.

  // Versioning (maps to PRAGMA user_version for SQLite; a row/file elsewhere).
  virtual uint32_t       schema_version() = 0;
  virtual void           set_schema_version(uint32_t) = 0;
};

// Factory: config decides. Default = sqlite.
//   [backend] type = sqlite | file | mysql
//   [backend] dsn  = mysql://user:pass@host/db   (remote, future)
std::unique_ptr<StorageBackend> open_backend(const std::string& index, const BackendConfig&);
```

Implementations:
- **`SqliteBackend`** — default. One `<index>.db`, schema below. WAL + FULL.
- **`FileBackend`** — wraps the CURRENT per-file stores unchanged. Kept one release
  as fallback + as the behavior-preserving baseline to diff against during migration.
- **`RemoteSqlBackend`** (future) — mysql/mariadb via DSN, for shared/team indexes.
  Same interface; only the factory + a driver change. Config: `type=mysql, dsn=...`.

Config selection is per index (a personal index can be local sqlite while a shared
team index is remote), resolved by `open_backend`. Callers are backend-agnostic.

## Schema (SqliteBackend: one `<index>.db` per index)

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;           -- durability for precious data (FULL over NORMAL: safer)
PRAGMA application_id=<gogol magic>;
PRAGMA user_version=<INDEXER_VERSION>;   -- the versioning we designed, native

CREATE TABLE entries(              -- was .meta + .emb
  rowid INTEGER PRIMARY KEY,
  proto INT, path TEXT, chunk TEXT, line INT, end_line INT, hash INT,
  embedding BLOB                   -- f16/f32 vector; loaded to RAM at startup
);
CREATE TABLE notes(                -- was .mem  (PRECIOUS)
  topic TEXT PRIMARY KEY, content TEXT, timestamp INT, sources TEXT
);
CREATE TABLE glossary(term TEXT PRIMARY KEY, expansion TEXT);   -- was .glossary (PRECIOUS)

-- Unified edge model (was .calls, .imports, .docrefs, .types)
CREATE TABLE edges(
  kind TEXT,                       -- 'call' | 'import' | 'docref' | 'type'
  src TEXT, dst TEXT, file TEXT, line INT, attrs TEXT   -- attrs = JSON for per-kind extras
);
CREATE INDEX edges_src ON edges(kind, src);
CREATE INDEX edges_dst ON edges(kind, dst);   -- reverse lookups (callers_of / referenced_by)

CREATE TABLE exports(file TEXT, symbol TEXT);
CREATE TABLE metrics(file TEXT, name TEXT, complexity INT, lines INT,
                     params INT, returns INT, max_depth INT);

CREATE VIRTUAL TABLE entries_fts USING fts5(   -- was .kw (BM25)
  path, chunk, content, content='entries', content_rowid='rowid'
);
```

Note: the `edges` table realizes the "gogol is a graph" insight — calls/imports/
docrefs/types collapse into one table + two indexes. Adding a new edge kind = a new
`kind` value, no schema change. This is the extensibility litmus test made concrete.

## Adoption steps (ordered, each independently shippable)

**Phase 0 — Vendor + build (no behavior change).**
- Add `vendor/sqlite/sqlite3.c` (+ `sqlite3.h`) and `vendor/sqlite-vec/sqlite-vec.c`.
- CMake: compile both into the `gogol` target; `-DSQLITE_ENABLE_FTS5`.
- Verify: builds, links, `sqlite3_libversion()` callable. No stores touched yet.

**Phase 1 — Introduce the `StorageBackend` adapter + `FileBackend` (behavior-preserving).**
- Define the `StorageBackend` interface + `IndexData` aggregate + `open_backend`
  factory (default `file`).
- Implement `FileBackend` by wrapping the EXISTING per-file store functions verbatim
  (load_index, load_call_graph, load_mem_store, …). No on-disk change.
- Route the daemon/indexer/CLI through `open_backend(...)` instead of calling the
  store functions directly. This is the seam that makes SQLite swappable later.
- Verify: full behavior parity with today (all commands, all tests) — pure refactor.

**Phase 2 — `Db` wrapper + schema (SqliteBackend groundwork).**
- Thin RAII C++ wrapper over `sqlite3*` (open, exec, prepared stmts, transaction
  RAII). Apply PRAGMAs. Create schema. `user_version` read/write.
- Verify: open a scratch `<tmp>.db`, create schema, round-trip a row.

**Phase 3 — `SqliteBackend`, behind config `type=sqlite`.**
- Implement `load_all`/`save_all` (+ fine-grained put/del) against the schema,
  loading into the SAME in-memory structs. `save_all` in ONE transaction.
- File backend stays default; SQLite opt-in per index.
- Verify on `docrefs_test`: query/explore identical between `file` and `sqlite`.

**Phase 4 — Migration importer (`gogol migrate <index>`).**
- Reads existing per-index files via `FileBackend.load_all()` → writes one
  `<index>.db` via `SqliteBackend.save_all()` in a single transaction →
  **verifies round-trip** (counts + spot-check, esp. notes) → renames old files to
  `<name>.<ext>.premigrate` (kept, not deleted).
- PRECIOUS FIRST: notes + glossary migrated and verified before anything else; abort
  the index migration if their round-trip check fails.
- Idempotent + resumable: if `<index>.db` exists and `user_version` current, skip.
- Backup: snapshot `~/.gogol/indexes/` before running (as done 2026-08-30).

**Phase 5 — Flip default to SQLite; atomicity win lands.**
- `open_backend` default becomes `sqlite`. `save_all` in one transaction eliminates
  the mid-index cross-store inconsistency (a kill mid-index rolls back cleanly).
- `FileBackend` retained behind `type=file` for one release as fallback.

**Phase 6 — Cleanup / later.**
- After confidence: remove `.premigrate` files (`gogol prune --migrated`).
- Optional: `sqlite-vec` for in-DB vector scan if an index grows enough to matter.
- Optional: `RemoteSqlBackend` (mysql/mariadb via `dsn=`) for shared team indexes —
  new backend impl + factory case only; no caller changes.

## Migration safety guarantees

- Backup taken before migration; old files kept as `.premigrate` until user prunes.
- Precious stores (notes, glossary) migrated first, round-trip verified, abort-on-mismatch.
- Round-trip verification: row counts match + content spot-check per table.
- Idempotent (safe to re-run) and resumable (skips already-migrated indexes).
- Derived stores can always be rebuilt by `gogol index --force` as ultimate fallback.

## Versioning, finally resolved

`PRAGMA user_version` = the per-index indexer version. On open: if
`user_version < INDEXER_VERSION`, re-extract derived tables (edges/exports/metrics/
fts) — WITHOUT re-embedding (embeddings keyed by their own stored dim/precision).
This is the "backfill docrefs without re-embed" behavior, now native to SQLite.
The separate `.version` file idea is dropped — `user_version` supersedes it.

## Notes on the immediate fixes

The standalone `INDEXER_VERSION` file + `fsync` patches discussed earlier are
SUPERSEDED by this plan (`user_version` + SQLite transactions/WAL give both). If
doc-refs backfill is needed before this epic lands, the stopgap is a one-time
`gogol index --force` per index — no throwaway code.
