---
name: gogol
description: >
  Code search, call-graph navigation, and knowledge store. Use for finding code,
  tracing function relationships, storing findings, and navigating unfamiliar codebases.
user_invocable: true
---

# gogol

Semantic search and knowledge store backed by local embeddings. Daemon should be running (`gogol serve`).

## Decision Flow

Start here. Pick the right tool for the intent:

| Intent | Command | Why |
|--------|---------|-----|
| Understand a specific function | `gogol explore` | One call: signature, source, callers, callees, imports, related |
| Find code by concept/meaning | `gogol query` | Semantic + keyword hybrid search |
| Trace call chains structurally | `gogol calls` | AST-extracted, no source reading needed |
| Find blast radius of a change | `gogol affected` | Transitive import graph traversal |
| Read source at a known location | `gogol get` | Direct retrieval by location |
| Find exact strings/identifiers | `grep` | Literal match, not semantic |

**Key rule:** If query found the right location, `get` or `explore` it. Don't query again with different terms hoping for better output — retrieve what you found.

**grep anti-pattern:** If your grep pattern is an alternation of guessed names (`fooHandler|handleFoo|onFoo`, `user.?[Ss]ession|UserSession`), stop. The alternation means you don't know the exact string — that's a semantic search. Use `gogol query "<the intent>"` instead. Widening the alternation after an empty result (adding more `|` branches) is the failure signal: switch tools, don't add branches.

Exact known strings (a specific symbol, error message, or `^export type Foo =`) are correct grep usage. The tell is the `|`: one concept, one string → grep; one concept, many guessed strings → `gogol query`.

## Explore (default for function investigation)

One-call deep dive. Replaces query + get + calls with a single ~2KB result.

```bash
gogol explore "processOrder" --index backend   # full function context
gogol explore "checkForAlerts" --index backend       # definition + call graph + deps
gogol explore "handleEvent" -n 30                    # show 30 lines of source
```

Output: signature, source snippet, file imports, callers, callees (project-only), related symbols.

**Use explore when:** you know the function name and need to understand it. Skip query entirely.

For a **doc/markdown** entry, explore instead shows its reference graph (see below).

## Doc references

Markdown links `[text](target)` in `.md` files are extracted into a reference graph and surfaced by `explore` on a doc, and as counts in `query`.

```bash
gogol explore "OVERVIEW" --index backend --file docs/OVERVIEW.md
```

Sections in the output:
- `references:` — local docs this file links to (`<title> → path`, round-trips into `gogol get`)
- `external references:` — off-index URLs (Jira/GitLab/etc., `<title> ↗ url`) — read with `web_fetch`
- `referenced by:` — docs that link to this one (reverse graph, local only)

`query` appends a compact `[refs:N ext:M refby:K]` suffix to each doc result (space-separated in agent mode). Only local links that resolve to an indexed file become `references`; unresolvable/out-of-project links are dropped; `://` links become `external references`. The link text is used as the title.

## Query

Semantic + keyword hybrid search with RRF ranking.

```bash
gogol query "payment capture flow"                   # search all indexes
gogol query "auth token refresh" --index web -n 10   # specific codebase
gogol query "error handling" --index backend         # scope to one index
gogol query "config" --show                          # show code snippets inline
```

### Query Refinement

When results aren't right:

| Problem | Action |
|---------|--------|
| Too noisy / irrelevant hits | Narrow with `--index web` or `--type doc` |
| Too few results | Broaden terms, try synonyms, drop `--index`/`--type` filters |
| Right file but wrong function | `gogol get` the location, or `gogol explore` the function name |
| Need structural context | Switch to `gogol calls` or `gogol explore` |

**Don't:** query → query → query with slightly different terms. Two queries max, then switch strategy.

### Filtering by Type

Restrict results to a single entry type with `--type`:

```bash
gogol query "auth" --index web              # all types in web index
gogol query "auth" --type doc --index web   # only file (doc) entries
gogol query "deploy" --type note            # only memory notes
gogol query "term" --type term              # only glossary terms
```

Types: `doc` = file chunk, `note` = memory note, `term` = glossary term. Omit `--type` for all types.

## Calls

AST-extracted call graph. No source reading needed.

```bash
gogol calls "handleEvent" --index backend            # callers + callees
gogol calls "processOrder" --depth 3                  # recursive tree (3 levels)
gogol calls "hashValue" --out                        # only what it calls
gogol calls "hashValue" --in                         # only who calls it
```

**Use calls when:** you need to trace execution flow across files without reading them all.

## Affected

Find all files that transitively depend on given files via the import graph.

```bash
gogol affected "order-hash.ts" --index backend                  # all dependents
gogol affected "order-hash.ts" --filter "*.test.*"              # only test files
gogol affected "orders.ts" --filter "e2e/**"                  # only e2e specs
git diff --name-only | gogol affected --stdin --index backend   # from git diff
```

## Get

Direct retrieval by location. Use after query/list finds the right entry.

```bash
gogol get note web auth/flows            # memory note content
gogol get doc web src/utils.ts:42        # file from line 42 (full)
gogol get doc web src/utils.ts:42 -n 30  # 30 lines from line 42
gogol get term backend OMS               # glossary expansion
```

## Metrics

Function complexity profile for a file.

```bash
gogol metrics "orders.ts" --index backend
gogol metrics "order-hash.ts" --index backend
```

## Knowledge Store

### Store findings during investigation

Don't wait until session end. After understanding something non-trivial (traced a flow, found a gotcha, resolved ambiguity) — store it immediately.

```bash
# Memory note (works on any index, regardless of mode)
gogol add note team deploy/k8s "HPA scales on CPU at 70%" --sources "docs/infra.md"

# Glossary term (improves search for abbreviations)
gogol add term backend OMS "order management system, order processing"

# Multi-line via heredoc
gogol add note team payments/flow --stdin <<'EOF'
1. Customer initiates payment
2. Backend creates a payment provider session
3. Redirect to the provider hosted page
EOF
```

### List and Remove

```bash
gogol list web                           # all entries in web (all types)
gogol list note team                     # memory notes
gogol list term backend                  # glossary terms
gogol rm note web outdated-topic         # remove memory note
```

## Entry Types

Every entry is addressed as `<type> <index> <path>`:

| Type | Meaning | Example |
|------|---------|---------|
| `doc` | File chunk under an index's configured paths | `doc web src/utils.ts:42` |
| `note` | Memory note in the index's store | `note web auth/flows` |
| `term` | Glossary term (term → expansion) | `term web OMS` |

Query output leads with the type token and is copy-pasteable straight into `get`:

```
doc  web src/auth/token.ts:42 § handleLogin
note web auth/session-notes
term web JWT
```

## Output Format

Auto-selects agent mode when piped. Override: `gogol --format=agent` or `--format=default`.
Path control: `--path full|abs|short` (`--abs` is alias for `--path abs`).

## Context Efficiency

gogol results are ~500-2000 chars per call. For comparison:
- `grep`: ~500-1100 chars but requires follow-up reads
- `read` tool: ~1600-8400 chars per file section

**One `explore` call (~2KB) replaces a query + get + calls sequence (~4-6KB total).** Prefer explore when you know the function name.
