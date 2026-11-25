// refresh.h — on-demand ("refresh") indexing shared by the CLI and the MCP server.
//
// The single entry point is refresh_repo(): given a repo toplevel it resolves the
// git-identity index name, synthesizes the configless IndexConfig (whole toplevel,
// default extensions), and runs the incremental disk sync (resync_disk_index) which
// re-embeds ONLY files whose stat-hash changed since the last index (the file_hashes
// delta — unchanged files are skipped by the scanner's stat-hash gate).
//
// An OPTIONAL time budget (deadline) makes the per-FILE loop stop early once the
// deadline passes. Because a file's stat-hash is persisted only AFTER all its chunks
// are ingested, an interrupted refresh leaves the unfinished files unmarked, so the
// NEXT refresh naturally resumes them (no separate cursor). deadline == steady_clock
// time_point{} (the default) means "no budget" — run to completion (unbounded).
//
// Callers differ only in the budget:
//   * CLI  = halt-until-indexed: refresh_repo(..., no deadline) — blocks until done.
//   * MCP  = inline 5s budget (kMcpInlineRefreshBudgetMs) + async spillover: run
//     inline to the deadline so fresh changes show in THIS response, then if it hit
//     the budget, finish the rest on a detached background thread (owning its OWN
//     store + embedder — see mcp_server.cpp) so the call stays responsive.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>

#include "config/config.h"

class EmbedProvider;
namespace store { class Store; }

namespace gogol {

// Outcome of an ingest/refresh pass. `hit_budget` is true iff the loop stopped
// early because the deadline passed (there is unfinished work a follow-up refresh
// will resume); false means it ran to completion.
struct IngestResult {
    int read = 0;      // chunks scanned
    int ingested = 0;  // chunks put/touched
    int purged = 0;    // stale chunks removed
    int gc_purged = 0; // DELETED files whose stored traces were GC'd this pass
    bool ok = true;
    bool hit_budget = false;
    std::string err;
};

// The MCP inline-refresh time budget: how long a query's INLINE refresh may run
// before the remainder spills over to a background thread. The CLI does not use
// this (it refreshes unbounded). One named constant — no bare 5000 elsewhere.
inline constexpr std::chrono::milliseconds kMcpInlineRefreshBudgetMs{5000};

// A steady_clock deadline; the default-constructed value means "no budget".
using Deadline = std::chrono::steady_clock::time_point;

// Progress callback: (current_chunk, total_chunks, current_file). Called at file
// boundaries during indexing. Return false to abort (not currently used).
using ProgressCallback = std::function<bool(int current, int total, const std::string &file)>;

// Ingest one index's files STRAIGHT FROM DISK into the store incrementally: the
// FileAdapter scans + chunks, changed files are re-embedded, unchanged files are
// skipped via the stat-hash gate, and stale chunks/edges are reconciled. When
// `deadline` is set (non-default), the per-file loop stops before starting a new
// file once `now >= deadline`; the already-finished files are persisted and
// r.hit_budget is set true. Reuses the file_hashes delta — it does NOT reimplement
// diffing.
IngestResult resync_disk_index(store::Store &store, EmbedProvider &embedder,
                               const std::string &name, const IndexConfig &cfg,
                               const std::string &repo_root = "",
                               Deadline deadline = Deadline{},
                               ProgressCallback progress = nullptr,
                               const std::string &root_sha = "");

// Refresh a repo by its git identity: use the provided `common_dir` as the index
// identity (the worktree-agnostic git path that becomes entries.idx). If `common_dir`
// is empty, it's resolved via gogol::resolve_git_identity. Synthesizes a configless
// IndexConfig (whole toplevel + default extensions), and runs resync_disk_index with
// the given (optional) deadline. This is the ONE shared refresh entry used by both
// the CLI (no deadline => block until done) and the MCP server (inline budget +
// spillover). `repo_toplevel` must be a git repo toplevel (absolute); an empty or
// invalid one yields r.ok=false.
IngestResult refresh_repo(store::Store &store, EmbedProvider &embedder,
                          const std::string &repo_toplevel,
                          const std::string &common_dir = "",
                          Deadline deadline = Deadline{},
                          ProgressCallback progress = nullptr);

} // namespace gogol
