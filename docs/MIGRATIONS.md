# Transition migrations — REMOVED (historical record)

These were **one-time transition migrations** (M1–M7) added during the root-SHA
identity / ref-sightings / relative-paths work. They healed a pre-existing store to the
current schema+data shape on open (idempotent, state-guarded). Once every machine was
migrated they became dead weight and were **removed** — this file records what they did,
why removal was safe, and the permanent pieces that stayed.

**Status: removed** after all machines were confirmed fully migrated. The heal-on-open
code is gone; `grep -rn "TRANSITION MIGRATION" src/` returns nothing. New stores get the
final schema directly from `kSchema` / `kNotesSchema`; new rows are written in the final
shape at `put()` time.

## What they were

| ID | What it did | Removed from |
|----|-------------|--------------|
| **M1** | `ALTER entries ADD COLUMN root_sha` | store.cpp RW ctor |
| **M2** | abs→relative content paths (entries.source / entry_sources.source / edges.file) | store.cpp RW ctor |
| **M3** | `ALTER notes ADD COLUMN scope_id` | store.cpp RW ctor (notes) |
| **M3-ro** | same `scope_id` ADD for read-only opens | store.cpp `ensure_notes_schema_ro` |
| **M4** | `Store::backfill_root_sha` + its refresh.cpp call | store.cpp + refresh.cpp |
| **M5** | `Store::rekey_index` (path-idx → SHA-idx) + its refresh.cpp call | store.cpp + refresh.cpp |
| **M6** | note scope_path (absolute) → (scope_id = ref, scope_path = relative subdir) for single-root repos | store.cpp RW ctor (notes) |
| **M7** | `file_hashes` re-keyed to (idx, source) | store.cpp RW ctor |

## Permanent pieces kept (NOT part of the removal)

- The `root_sha` / `scope_id` columns (in `kSchema` / `kNotesSchema`; written at `put()`).
- The `entries_root_sha` and `notes_scope_id` indexes (now created unconditionally /
  folded into `kNotesSchema`).
- `file_hashes` with its `(idx, source)` PK (baseline in `kSchema`).
- `refresh.cpp`'s `path_idx` — the **fallback index identity** for a repo with no single
  root SHA (`idx = root_sha ? root_sha : path_idx`). This is load-bearing for multi-root /
  orphan repos; only the M4/M5 *calls* were removed, not `path_idx`.
- `kCacheSchemaVersion` was NOT lowered; no columns were dropped.

## Two corrections discovered during removal

1. **Multi-root repos stay path-keyed forever — that is correct, not unmigrated.**
   A repo with several root commits (or an orphan/rootless repo) has no single root SHA,
   so `resolve_root_sha` returns empty and `refresh_repo` keys it by path (`idx =
   path_idx`), with NULL `root_sha` and no sighting (sightings are SHA-gated). The old M5
   removal guard ("all idx are 40/64-hex") could therefore never be met while such a repo
   was indexed. The real guard: a path-form idx is unmigrated ONLY if its repo has a
   single root commit (`git -C <idx> rev-list --max-parents=0 HEAD | wc -l` == 1); a
   multi-root repo is the legitimate steady state.

2. **MIGRATIONS.md step 2 was wrong about `path_idx`.** It said "after removing M5,
   `path_idx` becomes unused — drop it." Wrong: `path_idx` is also the fallback idx
   (above), so it was KEPT. Only the `rekey_index` call was removed.

## Pre-removal verification (the gate that was met on every machine)

```sql
-- M2: no absolute content paths
SELECT COUNT(*) FROM entry_sources WHERE source LIKE '/%';   -- 0
SELECT COUNT(*) FROM entries       WHERE source LIKE '/%';   -- 0
-- M5: any path-form idx must belong to a MULTI-ROOT/non-single-root repo (checked by hand)
SELECT DISTINCT idx FROM entries WHERE idx LIKE '/%';
--   for each: git -C <idx> rev-list --max-parents=0 HEAD | wc -l  > 1  => legitimate
-- M4: no unstamped rows under a SHA idx
SELECT COUNT(*) FROM entries WHERE idx NOT LIKE '/%' AND root_sha IS NULL;  -- 0
-- M3/M6: any absolute note scope_path must be non-repo or multi-root (checked by hand)
SELECT DISTINCT scope_path FROM notes WHERE scope_id IS NULL AND scope_path LIKE '/%';
-- M7: file_hashes carries the idx column (PRAGMA table_info(file_hashes))
```

## If you ever need to re-heal an old store

The migrations are gone, but the store is derived/rebuildable: delete `cache.db` and let
it re-index (fresh stores are born in the final shape). Notes (`notes.db`) are the only
content of record; a legacy notes.db from before M3/M6 would keep its old-shape rows, but
all known stores were migrated before removal. If a truly ancient notes.db surfaces,
restore the M3/M6 blocks from git history (commit before the removal) for one open.
