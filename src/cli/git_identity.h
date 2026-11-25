// git_identity — git-identity resolution for gogol's path-based indexing.
//
// The insight (verified): `git rev-parse --git-common-dir` (absolute) is the SAME for a
// repo's main checkout AND every worktree of it — so it's a worktree-agnostic INDEX
// IDENTITY. `git rev-parse --show-prefix` is the CWD-relative subdir, i.e. the query
// SCOPE. Together they answer "which registered index am I in, and what subpath?" from
// any directory — main checkout or worktree, top or subdir — with zero absolute-path
// binding (the thing config-path models can't do across worktrees/moves).
#pragma once

#include "store/resolved_idx.h"

#include <optional>
#include <string>
#include <vector>

namespace gogol {

struct GitIdentity {
    std::string common_dir;  // absolute --git-common-dir: the index identity
    std::string toplevel;    // absolute --show-toplevel: this checkout's root
    std::string prefix;      // --show-prefix: CWD relative to toplevel (subdir scope),
                             // "" at the root, "services/backend/checkout/" in a subdir.
    std::string root_sha;    // root commit SHA (git rev-list --max-parents=0 HEAD):
                             // a CONTENT-derived, location-independent repo identity.
                             // Identical across moves/renames/worktrees (shared history)
                             // AND across the filesystem boundary (a container computes
                             // it from its OWN checkout). "" if unresolvable (empty repo,
                             // or ambiguous: >1 root commit from orphan/grafted history).
};

// Resolve the git identity for `dir` (defaults to CWD). std::nullopt if `dir` is not
// inside a git repo. Shells out to `git rev-parse` (git is the source of truth for
// worktree topology; reimplementing .git parsing would be fragile).
std::optional<GitIdentity> resolve_git_identity(const std::string &dir = "");

// Resolve ONLY the root commit SHA for `dir` (defaults to CWD). std::nullopt when the
// repo has no commits (empty) or an ambiguous root (>1 root commit) — callers treat an
// ambiguous/empty root as "no portable identity" and fall back to path/common_dir.
// A bare 40-hex SHA string is the portable scope token a container sends.
std::optional<std::string> resolve_root_sha(const std::string &dir = "");

// True iff `s` is a bare git object id (40 lowercase hex chars, or 64 for sha256 repos).
// Used by resolve_scope to tell a root-SHA scope token from a path / bare repo name.
bool looks_like_root_sha(const std::string &s);

// Strip a trailing "/.git" so a --git-common-dir matches the stored repo-toplevel form
// (INVARIANT §4: ONE normalizer, all sites). Returns `s` unchanged if it doesn't end
// in /.git. Idempotent.
std::string strip_git_suffix(const std::string &s);

// Resolve a path to a NOTE scope (ref + subpath), per the note-scoping model:
//  - in a SINGLE-root git repo: ref = root SHA, sub = path RELATIVE to the repo root
//    ("" when the path IS the root). abs stays empty. Survives clone/move.
//  - NOT in a resolvable repo (plain dir, or a MULTI-root repo with no single SHA):
//    ref/sub empty, abs = the absolute path. Matched by path-prefix (not portable).
//  - empty input: all empty (a global note).
struct NoteScope {
    std::string ref;  // repo root SHA, or "" when not a single-root repo
    std::string sub;  // subpath RELATIVE to the repo root when ref set (""=whole repo)
    std::string abs;  // absolute path when NOT a repo (ref empty); else ""
};
NoteScope resolve_note_scope(const std::string &path);

// A resolved scope: the full idx (common_dir, for code/doc entry filtering via
// ids_in_indexes) and the toplevel (for notes path-prefix filtering via
// note_scope_matches).
struct ResolvedScope {
    ResolvedIdx idx = ResolvedIdx::none();  // full index identity (toplevel path, no /.git suffix)
    std::string toplevel;   // repo toplevel (path-prefix for notes)
};

// Resolve a user-provided scope string to (idx, toplevel). Accepts, in order:
//   1. A path (absolute or relative-to-CWD) inside a git repo — uses
//      resolve_git_identity. This is the existing behavior.
//   2. A bare repo name ("godot-docs", "gogol") — reverse-matched against the
//      checkout PATH of every index in `known_indexes` (IndexLoc.path). First match
//      wins. (Matched against the path, not the idx: identity is now the root SHA, so
//      the repo NAME only lives in the path.)
//   3. A path suffix ("solo/godot-docs") — matched as a suffix of any known index's
//      checkout path. Fallback for when a bare name is ambiguous or a user gives a
//      partial path.
// A SHA is NOT a scope input: scope works on names/paths, a SHA is neither, so it
// simply won't resolve here — pass a portable identity via the dedicated `ref` param
// instead (scope and ref are different concepts, not two spellings of one).
// Returns std::nullopt if nothing resolves (NOT a sentinel struct — the
// caller must check has_value()). `known_indexes` is the store's index_locations()
// ((idx, checkout-path) pairs). This is the one user-facing scoping concept; CLI
// --scope and MCP scope both go through it.
std::optional<ResolvedScope> resolve_scope(const std::string &name,
                             const std::vector<IndexLoc> &known_indexes);


// --- Unified recall scope resolution (shared by CLI + MCP recall) ---------------
// The inputs a recall handler has, resolved ONCE into everything query_hybrid needs.
// Factored out of the two handlers (which had drifted — the MCP-parity bug) so the
// precedence + source_prefix + parent-dir rules live in ONE place.
struct RecallScopeInputs {
    std::string ref;        // --ref / `ref`: portable root-SHA identity ("" = none)
    std::string scope;      // --scope / `scope`: path / repo name / dir ("" = none)
    bool scope_set = false; // was scope explicitly provided? (CLI distinguishes "" global)
    std::string cwd;        // caller's CWD for the default-scope fallback ("" = none, e.g. MCP)
};
struct RecallScope {
    ResolvedIdx idx = ResolvedIdx::none();  // code/doc entry filter
    std::string note_scope;    // passed as query_hybrid scope_path (note matching: ref|path)
    std::string source_prefix; // checkout restriction (entry_sources.toplevel); "" when none
    std::string parent_dir;    // non-git dir to gather sighted refs under ("" = none)
    bool scoped = false;       // a single index/ref resolved (vs global / parent-dir)
    // Non-empty => the scope was INVALID and the caller must error out (not query). Set
    // when --scope looks like a path (absolute or contains '/') but resolves to no
    // indexed repo — like `ls <nonexistent>`: a bad path is an error, never a silent
    // fall-through to a whole-store union. A bare NAME that doesn't match is NOT an
    // error (it's a name lookup that may legitimately miss -> parent-dir/global).
    std::string error;
};
// `restrict_refs` is still built by the caller via Store::recall_restrict_refs(parent_dir,
// scoped) — that needs the store; this resolver is store-free (takes known_indexes).
RecallScope resolve_recall_scope(const RecallScopeInputs &in,
                                 const std::vector<IndexLoc> &known_indexes);

} // namespace gogol
