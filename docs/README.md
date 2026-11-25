# gogol docs

Documentation layout. Living references are current and fact-checked against the
code; historical records are point-in-time snapshots kept for context.

## Living references

| Doc | What it covers |
|-----|----------------|
| [../README.md](../README.md) | User-facing: install, setup, commands, storage, MCP integration |
| [../AGENTS.md](../AGENTS.md) | Developer/agent guide: project structure, build & verify loop, schema, hybrid search |
| [../SKILL.md](../SKILL.md) | Agent skill: when to use `recall`/`delve`/`links`/`metrics`/notes |
| [DESIGN.md](DESIGN.md) | The durable design record — what gogol *is*, what was proven, what was tried & killed (graveyard), what's designed-not-built |

(`README`, `AGENTS`, `SKILL` live at the repo root by convention — tooling and
contributors expect them there.)

## Historical records

Point-in-time; **not** maintained as current truth. See the living references above
for how things work now.

| Doc | What it records |
|-----|-----------------|
| [history/refactoring-cli-mcp-formatters.md](history/refactoring-cli-mcp-formatters.md) | The CLI+MCP unified-formatter refactor (only `recall` was migrated) |
| [history/refactoring-cli-mcp-plan.md](history/refactoring-cli-mcp-plan.md) | The (abandoned) plan to extend that refactor to the other commands |

## Elsewhere

- [../experiments/mem-v2/README.md](../experiments/mem-v2/README.md) — the reproducible
  proof harness that validated the v2 "memory-as-architecture" pivot (read-only
  experiments against copies of real indexes).

## Removed

The pi prompt-enrichment PoC and the `gogol precog` command were removed. Their docs
(investigation, summary, architecture, setup guides) and the pi-extension artifacts
were deleted; they remain available in git history if ever needed.
