#include "cli/git_identity.h"
#include "config/config.h"   // expand_tilde (shared)
#include "config/scanner.h"  // repo_tag

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace gogol {

namespace {

// Run a command in `dir` and return trimmed stdout, or nullopt on nonzero exit / no
// output. Uses popen (git is external); dir is switched via `git -C` so we never chdir
// the process (thread-safety, and no side effects on the caller's CWD).
std::optional<std::string> run_git(const std::string &dir, const std::string &args) {
    std::string cmd = "git ";
    if (!dir.empty()) {
        // -C <dir>: run as if git were started in <dir>. Expand a leading ~ (the shell
        // won't, inside our quotes), then single-quote to be safe.
        std::string d = expand_tilde(dir);
        cmd += "-C '";
        for (char c : d) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
        cmd += "' ";
    }
    cmd += args;
    cmd += " 2>/dev/null";

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;
    std::string out;
    std::array<char, 512> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        out.append(buf.data(), n);
    int rc = pclose(pipe);
    if (rc != 0) return std::nullopt;
    // trim trailing whitespace/newline
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    if (out.empty()) return std::nullopt;
    return out;
}

} // namespace

std::optional<std::string> resolve_root_sha(const std::string &dir) {
    // rev-list --max-parents=0 HEAD lists ALL root commits reachable from HEAD. A
    // normal repo has exactly one; orphan branches / grafted history can yield more.
    // >1 root => ambiguous identity, so we decline (nullopt) rather than pick one.
    auto out = run_git(dir, "rev-list --max-parents=0 HEAD");
    if (!out) return std::nullopt;           // no commits (empty repo) or not a repo
    // Single line == single root. A multi-line result means multiple roots.
    if (out->find('\n') != std::string::npos) return std::nullopt;  // ambiguous
    // Validate it looks like an object id before trusting it as an identity key.
    if (!looks_like_root_sha(*out)) return std::nullopt;
    return out;
}

bool looks_like_root_sha(const std::string &s) {
    // sha1 = 40 hex, sha256 = 64 hex. Lowercase (git emits lowercase).
    if (s.size() != 40 && s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string strip_git_suffix(const std::string &s) {
    if (s.size() > 5 && s.compare(s.size() - 5, 5, "/.git") == 0)
        return s.substr(0, s.size() - 5);
    return s;
}

NoteScope resolve_note_scope(const std::string &path) {
    NoteScope ns;
    if (path.empty()) return ns;  // global
    std::string p = expand_tilde(path);
    auto gid = resolve_git_identity(p);
    if (gid && !gid->root_sha.empty() && !gid->toplevel.empty()) {
        // Single-root repo: ref + subpath relative to the repo root. git's
        // --show-prefix already gives the CWD-relative subdir, but resolve_git_identity
        // takes a dir and returns the toplevel; relativize p against it ourselves so a
        // path ARG (not just CWD) works. Trailing '/' trimmed; "" means the repo root.
        std::string top = gid->toplevel;
        if (!top.empty() && top.back() == '/') top.pop_back();
        std::error_code ec;
        std::string cp = fs::weakly_canonical(p, ec).string();
        if (ec) cp = p;
        ns.ref = gid->root_sha;
        if (cp == top) ns.sub = "";                       // the repo root
        else if (cp.size() > top.size() + 1 &&
                 cp.compare(0, top.size(), top) == 0 && cp[top.size()] == '/')
            ns.sub = cp.substr(top.size() + 1);           // relative subpath
        else ns.sub = "";                                 // couldn't relativize -> whole repo
        return ns;
    }
    // Not a single-root repo (plain dir or multi-root): keep the absolute path.
    std::error_code ec;
    std::string cp = fs::weakly_canonical(p, ec).string();
    ns.abs = ec ? p : cp;
    return ns;
}

std::optional<GitIdentity> resolve_git_identity(const std::string &dir) {
    // --path-format=absolute makes --git-common-dir absolute (worktree identity must be
    // comparable regardless of CWD). Older git lacks --path-format; fall back to
    // canonicalizing the returned path ourselves.
    auto common = run_git(dir, "rev-parse --path-format=absolute --git-common-dir");
    if (!common) {
        auto rel = run_git(dir, "rev-parse --git-common-dir");
        if (!rel) return std::nullopt; // not a git repo
        std::error_code ec;
        fs::path base = dir.empty() ? fs::current_path(ec) : fs::path(dir);
        fs::path abs = fs::weakly_canonical(base / *rel, ec);
        common = ec ? *rel : abs.string();
    }
    auto top = run_git(dir, "rev-parse --show-toplevel");
    // --show-prefix returns "" at repo root (empty stdout ⇒ run_git gives nullopt);
    // that's a valid "root scope", so map nullopt → "".
    auto prefix = run_git(dir, "rev-parse --show-prefix");

    GitIdentity id;
    id.common_dir = *common;
    id.toplevel = top.value_or("");
    id.prefix = prefix.value_or("");
    id.root_sha = resolve_root_sha(dir).value_or("");
    return id;
}

std::optional<ResolvedScope> resolve_scope(const std::string &name,
                             const std::vector<IndexLoc> &known_indexes) {
    if (name.empty()) return std::nullopt;

    // `scope` is a path, a repo name, or a directory — a human handle for a location.
    // A SHA is a different concept (a portable identity): it belongs on the dedicated
    // --ref / `ref` param, not here. A SHA passed as scope is neither a path nor a known
    // repo name, so it simply won't resolve (falls through to the name/suffix miss path).

    // 1. Try as a path inside a git repo (existing behavior). This handles
    //    absolute paths, relative-to-CWD paths, and repo subdirs. A bare name
    //    like "gogol" (no slash, not a real dir) falls through to the name
    //    match below rather than shelling out `git -C gogol` from CWD.
    {
        std::error_code ec;
        fs::path as_path = name;
        bool looks_like_path = as_path.is_absolute() ||
                               name.find('/') != std::string::npos ||
                               fs::is_directory(as_path, ec);
        if (looks_like_path) {
            if (auto gid = resolve_git_identity(name)) {
                ResolvedScope out;
                // Identity = root SHA (T6): a path resolves to its repo's root-commit
                // SHA, which is what entries.idx is now keyed on — so recall from any
                // clone/location/move of the repo hits the one SHA-keyed index.
                // Fallback to the path (common_dir) when the root is unresolvable
                // (empty/orphan repo), matching the indexing-side fallback (R5).
                out.idx = ResolvedIdx::already_resolved(
                    !gid->root_sha.empty() ? gid->root_sha : strip_git_suffix(gid->common_dir));
                // toplevel stays the PATH (this checkout) — the per-checkout overlay
                // key for worktree/source scoping (R3). Identity by SHA, location by path.
                out.toplevel = gid->toplevel;
                return out;
            }
        }
    }

    // 2. Bare name: reverse-match against the CHECKOUT PATH of every known index
    //    (IndexLoc.path), via its trailing component. repo_tag("/home/u/solo/godot-docs")
    //    == "godot-docs". First match wins. Matched against the path, NOT the idx:
    //    entries.idx is the root SHA now, so the repo NAME only lives in the path
    //    (a repo with no sighting has path==idx==SHA and simply won't name-match).
    for (const auto &loc : known_indexes) {
        if (repo_tag(loc.path) == name) {
            ResolvedScope out;
            out.idx = ResolvedIdx::already_resolved(loc.idx);
            out.toplevel = strip_git_suffix(loc.path);
            return out;
        }
    }

    // 3. Path-suffix fallback: match "name" as a suffix of any known index's checkout
    //    path (e.g. "solo/godot-docs" matches "/home/u/solo/godot-docs"). Disambiguates
    //    colliding bare names and handles partial paths.
    for (const auto &loc : known_indexes) {
        std::string top = strip_git_suffix(loc.path);
        // suffix match: top ends with name, boundary is '/' or start
        if (top.size() >= name.size() &&
            top.compare(top.size() - name.size(), name.size(), name) == 0 &&
            (top.size() == name.size() || top[top.size() - name.size() - 1] == '/')) {
            ResolvedScope out;
            out.idx = ResolvedIdx::already_resolved(loc.idx);
            out.toplevel = top;
            return out;
        }
    }

    return std::nullopt;
}

RecallScope resolve_recall_scope(const RecallScopeInputs &in,
                                 const std::vector<IndexLoc> &known_indexes) {
    RecallScope out;
    const bool ref_explicit = !in.ref.empty();

    if (ref_explicit) {
        // --ref: the ref IS the idx (entries.idx = root SHA, notes.scope_id = ref).
        // A ref carries no checkout => no source_prefix. note_scope = the ref so
        // query-side note matching keys on scope_id.
        out.idx = ResolvedIdx::already_resolved(in.ref);
        out.note_scope = in.ref;
        // source_prefix stays empty.
    } else if (in.scope_set && !in.scope.empty()) {
        if (auto rs = resolve_scope(in.scope, known_indexes)) {
            out.idx = rs->idx;
            out.source_prefix = rs->toplevel;  // checkout root (code entry restriction)
            // note_scope = the ACTUAL path given (so note matching can extract the
            // subdir within the repo); a bare repo NAME (no slash) has no path -> use
            // the resolved toplevel.
            out.note_scope = (in.scope.find('/') != std::string::npos) ? in.scope
                                                                       : rs->toplevel;
        } else {
            // Didn't resolve to an indexed repo. If the scope LOOKS LIKE A PATH
            // (absolute or contains '/') it must exist on disk — like `ls <arg>`: a
            // nonexistent path is an ERROR, never a silent fall-through to a whole-store
            // union. An EXISTING dir that isn't one repo is a legit parent-dir (union
            // of the repos under it). A BARE NAME that didn't match is NOT a path, so
            // it's a name-lookup miss (falls through to parent-dir/global, no error).
            std::error_code ec;
            bool path_shaped = fs::path(in.scope).is_absolute() ||
                               in.scope.find('/') != std::string::npos;
            if (path_shaped && !fs::is_directory(in.scope, ec)) {
                out.error = "scope path does not exist: " + in.scope +
                            " (use a repo name, an existing directory, or --ref <sha>)";
                return out;
            }
            out.note_scope = in.scope;  // unresolved name / existing parent dir
        }
    } else if (!in.scope_set && !in.cwd.empty()) {
        // Default: the CWD's repo (CLI only — MCP passes empty cwd, so this is skipped).
        if (auto gid = resolve_git_identity(in.cwd)) {
            if (!gid->toplevel.empty()) {
                out.note_scope = gid->toplevel;
                out.source_prefix = gid->toplevel;
                std::string cwd_idx = strip_git_suffix(gid->common_dir);
                out.idx = ResolvedIdx::already_resolved(
                    !gid->root_sha.empty() ? gid->root_sha : cwd_idx);
            }
        }
    }

    out.scoped = !out.idx.value().empty();

    // Parent-dir multi-repo search: only when nothing single-index resolved and we are
    // not in a --ref lookup. The dir is the given scope, else the CWD (if any).
    if (!out.scoped && !ref_explicit) {
        out.parent_dir = (in.scope_set && !in.scope.empty()) ? in.scope : in.cwd;
    }
    // A --ref lookup (and, later, a multi-ref restriction) carries no single checkout,
    // so source_prefix must not apply; the caller clears it again after adding overlay
    // refs. Here we clear it for the ref case up front.
    if (ref_explicit) out.source_prefix.clear();
    return out;
}

} // namespace gogol
