# gogol: index identity, location, and scoping

How gogol identifies repositories, tracks where they live, stores file paths, and
scopes search. Reference for the current design.

---

## Two layers: identity and location

gogol separates **what a repo is** from **where it is**.

- **Identity** — the repo's **root commit SHA** (`git rev-list --max-parents=0 HEAD`).
  Content-derived, so it is identical across clones, worktrees, and directory moves
  (shared history), and derivable from any checkout without a shared filesystem.
  This is `entries.idx`.
- **Location** — the absolute paths where a repo and its files sit on this machine.
  Machine-specific and changeable. Stored separately, resolved at query time.

Everything else follows from keeping these apart.

---

## Index identity (`entries.idx`)

Each entry is tagged with its repo's root SHA. Resolution of a repo to its identity:

| Repo shape | `idx` |
|------------|-------|
| Single-root git repo | its root commit SHA |
| Worktree | the origin's root SHA (shared history) → same index as the main checkout |
| Two clones of one upstream | the same root SHA → the same index |
| Moved directory | unchanged root SHA → same index (no re-embed) |
| Multi-root repo (grafted/merged history) | falls back to the path identity (no single SHA) |
| Empty repo / not a git repo | falls back to the path identity |

Consequences:

- **Moving a repo costs nothing.** The identity is unchanged; on the next indexed
  recall the entries are re-keyed to the SHA if they were under an old path identity,
  and embeddings are reused (the stat-hash gate sees unchanged files).
- **Clones and worktrees of one repo are one index.** The last indexer's scan is what
  the index reflects. Per-checkout views are handled by the location layer below.
- **Multi-root and non-git repos keep working** on the path identity — no portability
  across machines, but no regression.

A repo can be scoped these ways, all resolving to the same identity:

- running gogol inside the repo (CWD),
- `--scope <path>` / MCP `scope: <path>` (also a repo name, or a directory holding
  several repos),
- `--ref <root-sha>` / MCP `ref: <sha>` — a dedicated param carrying the portable
  identity a container sends when it has no shared filesystem. Read-only DB lookup: no
  git shell-out, no refresh, no checkout filter.

`scope` and `ref` are two DIFFERENT concepts for two different jobs, not alternate
spellings of the same thing:
- `scope` answers "which repo/dir do I mean?" by a human handle — a path, a repo name,
  or a parent directory. It resolves against the local filesystem / known checkout paths.
- `ref` answers "this exact repo, by its portable root-SHA identity" — no path, no
  filesystem, just the key. For when there is no shared filesystem to name a path on.

They don't overlap: a SHA isn't a `scope` input (scope works on names/paths, and a SHA
is neither — it just won't resolve as a scope), and a path/name isn't a `ref`. Pick the
one that matches what you have: a location you can name → `scope`; a bare identity token
→ `ref`.

---

## Location layer: `ref_sightings`

```
ref_sightings(root_sha, path, last_seen, overlay)
```

A derived, rebuildable map from a repo's identity to the on-disk paths it has been seen
at, recorded best-effort whenever gogol resolves a repo. It is a **helper, never
identity** — losing it costs only re-learning.

Sightings are **hints verified on use**: before a sighted path is trusted (for display,
scoping, move detection) it is `fs::exists`-checked (microseconds). A path that no
longer exists is skipped. This is why stale sightings are harmless — nothing is trusted
without a stat.

What it provides:

- **Display** — results show a live checkout path reconstructed from the SHA idx, not
  the raw hash.
- **Parent-directory search** — running gogol in a directory that is *not* itself a git
  repo (e.g. `~/work`, which holds `web/`, `backend/`, …) scopes recall to the repos
  **sighted under that directory**, stat-filtered to live ones. The result is bounded to
  those child repos, not a global search.
- **Move detection** — a ref sighted at an old path (now absent) and a new path (present)
  resolves to the live location. There is no concept of an "original" — only live vs
  dead, decided by `fs::exists`.
- **Overlay** — see below.

---

## File paths are repo-relative

Content paths are stored **relative to the repo root**, never absolute:

- `entries.source`, `entry_sources.source`, `file_hashes.source`, `edges.file` → relative
  (`src/a.ts`).
- `entry_sources.toplevel` and `ref_sightings.path` → absolute. They are the location
  anchor that relative paths attach to.

At query time the absolute path is reconstructed as
`<resolved checkout toplevel> + "/" + <relative source>`. So recalling from the origin
shows the origin's path, from a clone shows the clone's path, from a moved directory
shows the new path — the display always matches where you are.

Filesystem operations reconstruct `root + relative` at the point they touch disk:

- the **incremental-sync gate** stats `root + rel` to skip unchanged files (its key is
  the same relative path the scanner emits — no path juggling),
- the **deleted-file GC** stats `root + rel` to confirm absence before purging; it only
  deletes on *confirmed* absence (a stat error never deletes), and it is scoped to the
  index's entries (a relative source is inherently repo-scoped).

---

## Scoping a query

A query resolves its scope to a repo identity, then filters:

- **`--ref <sha>`** — the dedicated portable-identity param: filters directly to that
  repo (entries + notes), read-only, no refresh, no checkout filter.
- **`--scope` / CWD** — a path, repo name, or directory, resolved to the repo's ref.
- **Code/doc entries** — filtered to the resolved ref (`entries.idx` / `root_sha`).
- **Parent-dir case** — when the scope is a non-git directory, the query scopes to the
  union of refs sighted under it (live only).
- **Notes** — scope-filtered (see below).
- **Overlay** — any repo flagged as an active overlay is unioned into the candidate set,
  on top of the resolved scope. Overlays never widen a fully-global search into an
  overlay-only one.

`status` reports a per-index breakdown: each index's path, entry count, and `root_sha`
(the portable token a connected client can discover and pass as a scope).

---

## Overlay

`gogol overlay [path]` marks a repo as an **active overlay** (a flag on its sighting,
default target CWD); `gogol overlay --stop [path]` clears it. While active, the repo's
content is **unioned** into recall from elsewhere — "also search this repo on top of
where I am." The flag is persistent (stored in `ref_sightings.overlay`), which is how
start/stop state survives between invocations in a daemon-less tool.

It is a plain union — no precedence or shadowing. It reuses the same per-checkout view
mechanism (`entry_sources.toplevel`) that worktrees use; the only addition is the
user-settable flag.

---

## Notes

Notes live in a **separate database (`notes.db`)**, the content of record, which no
cache rebuild / re-index / identity change ever touches. This two-DB split is why
destructive cache operations never risk notes.

A note's columns: `content`, `embedding`, `scope_path`, `scope_id`, `created`.

Scoping follows the **same ref mechanism as indexed content**, with one distinction:
notes are never part of the overlay union. A note has three shapes:

- **Global** (no `scope_id`, no `scope_path`) → surfaces for every query.
- **Repo-scoped** (`scope_id` = repo root SHA; `scope_path` = a subpath RELATIVE to the
  repo root, or NULL for the whole repo) → surfaces for that repo from any
  clone / move / worktree, narrowed to the subdir when set. Because the subpath is
  relative, it survives a move — the same ref+relative split as `entries.idx` +
  `entries.source`.
- **Non-repo** (`scope_id` NULL; `scope_path` = an ABSOLUTE path) → a note on a plain
  directory or a multi-root repo that has no single root SHA. Matched by absolute
  path-prefix; works by path but not across clones/moves.

So `scope_path`'s meaning is conditional on `scope_id`: a relative subpath when a ref is
present, an absolute path when not. The query resolves its scope once to
`(ref, relative-subpath, absolute-path)` — the same resolution code uses — and a note
surfaces per its shape: global always; repo-scoped iff same ref and the query is at/under
the note's subdir; non-repo iff the query's absolute path is at/under the note's path.

Multi-root repos (ambiguous root → no single SHA) fall into the non-repo case — scoped
by absolute path, no ref tiebreak needed.

---

## Migrations

Schema and data transitions are one-time, heal-on-open migrations: idempotent,
state-guarded, and self-healing across machines. They convert a pre-existing store to
the current shape (add `root_sha` / `scope_id`, re-key path idx → SHA, rewrite absolute
content paths → relative, backfill note `scope_id`).

They are tagged in source (`grep -rn "TRANSITION MIGRATION" src/`) and inventoried in
`docs/MIGRATIONS.md` with the exact SQL check that proves a store is past each one and
the removal procedure — they become dead weight once every store is migrated. cache.db
is derived/rebuildable; notes.db migrations are strictly additive (content, embeddings,
and `scope_path` are never mutated).

---

## Schema summary

**cache.db** (derived / rebuildable):

```
entries(id, content, embedding, source, idx, root_sha, line, end_line, symbol, <metrics>)
  idx      = root commit SHA (identity)        source = repo-relative path (location)
entry_sources(entry_id, toplevel, source)      toplevel = absolute checkout root
file_hashes(idx, source, hash)                  (idx, source) key — relative source,
                                                repo-scoped (sync gate)
edges(kind, src_sym, dst_sym, idx, file)        file   = repo-relative
ref_sightings(root_sha, path, last_seen, overlay)   path = absolute (location + overlay flag)
entries_fts / entries_vec                       keyword + ANN search
```

**notes.db** (content of record):

```
notes(id, content, embedding, scope_path, scope_id, created)
  scope_path = absolute (host scoping)   scope_id = root SHA (portable scoping)
notes_vec                                 ANN search
```
