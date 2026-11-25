#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Returns pairs of (relative_path, change_hash)
// change_hash is based on mtime+size (fast, no file read)
//
// `repo_root` enables git-identity nested-repo-boundary skip: when non-empty, a
// subdirectory that is the TOP of a DIFFERENT git repo (it contains a `.git`
// entry AND is not `repo_root` itself) is NOT descended into — its files belong
// to that nested repo's own index, not this one. Empty (the default) disables the
// check, preserving the config-path scan behavior unchanged. Directory symlinks
// are always skipped (never descended) so a symlinked tree can't smuggle files
// in or cause a cycle.
std::vector<std::pair<std::string, uint64_t>>
scan_md_files(const std::filesystem::path &root,
              const std::vector<std::string> &extensions = {".md"},
              const std::filesystem::path &repo_root = {});

// True if `dir` is the top of a git repository that is DISTINCT from the scan
// root — i.e. it contains a `.git` entry (dir or file, the worktree/submodule
// form) and is not `repo_root` itself. A nested repo is a separate index
// identity; the parent scan must not descend into it (prevents a repo-of-repos
// leaking child files into the parent index). `repo_root` empty => always false.
bool is_nested_repo_boundary(const std::filesystem::path &dir,
                             const std::filesystem::path &repo_root);

// Derive a stable, bare index NAME from a git repo toplevel path. The name is the
// basename of the toplevel (e.g. "/home/u/projects/backend" -> "backend"), with a
// trailing slash tolerated. This is used for display (repo_tag is the preferred form
// under the git-identity model). Returns "" for an empty input.
std::string git_index_name(const std::filesystem::path &toplevel);

// Derive a human-readable display tag from an idx (which is now the git common_dir
// path under the common_dir identity model). For a common_dir like
// "/Users/joe/projects/backend/.git" the tag is "backend". For a bare repo
// "/srv/repos/backend.git" also returns "backend". Use this for display and for
// --index <label> resolution (match the tag, store the full path).
std::string repo_tag(const std::string &idx);

std::string read_file(const std::string &path);

// True if `abs_path` (a file under `index_root`) is excluded from indexing —
// i.e. matches a pattern in .gitignore OR .gogolignore (same syntax; the latter
// is applied on top, at global + repo-root + nested-directory levels, exactly
// like .gitignore). Mirrors the scan-time dir-skip so "ignored" == "not indexed".
bool path_is_ignored(const std::filesystem::path &abs_path,
                     const std::filesystem::path &index_root);
