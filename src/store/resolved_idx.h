// ResolvedIdx — a type that can ONLY be produced by the index/scope resolvers.
//
// WHY THIS EXISTS: the store's index-scoped methods (links/literal_search/query_hybrid)
// filter by a full idx string. A user-provided --scope value is a BARE name, a path,
// or a path suffix — it must go through resolve_scope first, or the store exact-matches
// a bare name against the stored full-path idx and silently returns empty (bug d87e3b8:
// resolve was wired into recall but not delve/links/literal — three handlers passed the
// raw arg, each a silent-empty regression).
//
// Making the store take `const ResolvedIdx&` instead of `const std::string&` turns
// "forgot to resolve" from a runtime silent-empty into a COMPILE error at every call
// site at once: you cannot construct a ResolvedIdx except via the resolvers (private
// ctor + friends). An already-resolved idx (a stored full path, or empty = "no scope")
// enters via ResolvedIdx::already_resolved() — the one explicit escape, named so it's
// greppable and intentional, not accidental.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace gogol {

// An index (root-SHA `idx`) paired with a checkout PATH to match a user scope string
// against. Identity is the SHA; the human-facing repo NAME lives in the path, so the
// scope resolver matches a bare name / path-suffix against `path` and returns `idx`.
struct IndexLoc { std::string idx; std::string path; };

// Fwd-declare the resolver so it can befriend ResolvedIdx's private ctor.
struct ResolvedScope;
class ResolvedIdx;
std::optional<ResolvedScope> resolve_scope(const std::string &name,
                             const std::vector<IndexLoc> &known_indexes);

class ResolvedIdx {
public:
    // The resolved full idx string ("" means "no index scope" — match all).
    const std::string &value() const { return value_; }
    bool empty() const { return value_.empty(); }

    // The ONE explicit escape hatch: wrap a value already known to be a stored full
    // idx (or deliberately empty). Named + greppable so a reviewer sees the intent.
    // Use for: the empty "no scope" default, or an idx that came straight out of the
    // store (index_locations / an entry row) and so is resolved by construction.
    static ResolvedIdx already_resolved(std::string v) { return ResolvedIdx(std::move(v)); }

    // Convenience: the empty (no-scope) resolved idx.
    static ResolvedIdx none() { return ResolvedIdx(std::string{}); }

private:
    explicit ResolvedIdx(std::string v) : value_(std::move(v)) {}
    std::string value_;

    // Only the resolver may mint one from an UNRESOLVED user string.
    friend std::optional<ResolvedScope> gogol::resolve_scope(
        const std::string &, const std::vector<IndexLoc> &);
};

} // namespace gogol
