#include "config/scanner.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include <fnmatch.h>

namespace fs = std::filesystem;

// Convert a gitignore glob pattern to a regex pattern.
// Handles *, **, ?, and character classes.
static std::string glob_to_regex(const std::string& glob) {
    std::string re;
    re.reserve(glob.size() * 2);
    
    size_t i = 0;
    while (i < glob.size()) {
        char c = glob[i];
        
        if (c == '*') {
            if (i + 1 < glob.size() && glob[i + 1] == '*') {
                // ** matches anything including /
                if (i + 2 < glob.size() && glob[i + 2] == '/') {
                    // **/ matches zero or more directories
                    re += "(?:.*/)?";
                    i += 3;
                } else if (i == 0 || glob[i - 1] == '/') {
                    // Leading ** or /**
                    re += ".*";
                    i += 2;
                } else {
                    re += ".*";
                    i += 2;
                }
            } else {
                // * matches anything except /
                re += "[^/]*";
                i++;
            }
        } else if (c == '?') {
            re += "[^/]";
            i++;
        } else if (c == '[') {
            // Character class - pass through mostly unchanged
            re += '[';
            i++;
            if (i < glob.size() && (glob[i] == '!' || glob[i] == '^')) {
                re += '^';
                i++;
            }
            while (i < glob.size() && glob[i] != ']') {
                if (glob[i] == '\\' && i + 1 < glob.size()) {
                    re += '\\';
                    re += glob[++i];
                } else {
                    re += glob[i];
                }
                i++;
            }
            if (i < glob.size()) {
                re += ']';
                i++;
            }
        } else if (c == '\\' && i + 1 < glob.size()) {
            // Escape sequence
            re += '\\';
            re += glob[++i];
            i++;
        } else if (c == '.' || c == '(' || c == ')' || c == '+' || 
                   c == '|' || c == '^' || c == '$' || c == '{' || c == '}') {
            // Regex metacharacters that need escaping
            re += '\\';
            re += c;
            i++;
        } else {
            re += c;
            i++;
        }
    }
    return re;
}

// A single gitignore pattern with its matching context.
struct IgnorePattern {
    std::regex regex;
    bool negated = false;
    bool dir_only = false;
    bool valid = false;
    
    IgnorePattern(const std::string& pattern, bool neg, bool dir) 
        : negated(neg), dir_only(dir) {
        try {
            regex = std::regex(pattern, std::regex::optimize);
            valid = true;
        } catch (...) {
            valid = false;
        }
    }
};

// Parse a .gitignore file into patterns.
// base_path is the directory containing the .gitignore (relative to repo root).
static std::vector<IgnorePattern> parse_gitignore(const fs::path& file, 
                                                   const std::string& base_path) {
    std::vector<IgnorePattern> patterns;
    std::ifstream f(file);
    if (!f) return patterns;
    
    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing whitespace and \r (but not escaped spaces)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\t' ||
               (line.back() == ' ' && (line.size() < 2 || line[line.size()-2] != '\\')))) {
            line.pop_back();
        }
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;
        
        bool negated = false;
        bool dir_only = false;
        std::string p = line;
        
        // Handle negation
        if (p[0] == '!') {
            negated = true;
            p = p.substr(1);
        } else if (p[0] == '\\' && p.size() > 1 && (p[1] == '!' || p[1] == '#')) {
            // Escaped ! or # at start
            p = p.substr(1);
        }
        
        // Handle directory-only (trailing /)
        if (!p.empty() && p.back() == '/') {
            dir_only = true;
            p.pop_back();
        }
        
        if (p.empty()) continue;
        
        // Determine if pattern is anchored (contains / not at end)
        bool anchored = (p[0] == '/');
        if (anchored) p = p.substr(1);
        if (!anchored) anchored = (p.find('/') != std::string::npos);
        
        // Build the regex pattern
        std::string re_pattern;
        if (anchored) {
            // Anchored to base_path
            if (base_path.empty()) {
                re_pattern = "^" + glob_to_regex(p);
            } else {
                re_pattern = "^" + std::regex_replace(base_path, std::regex("\\."), "\\.") + "/" + glob_to_regex(p);
            }
        } else {
            // Can match in any directory - match at start or after /
            re_pattern = "(?:^|/)" + glob_to_regex(p);
        }
        
        // For directory patterns, ensure we match the dir and anything under it
        if (dir_only) {
            re_pattern += "(?:/.*)?$";
        } else {
            re_pattern += "(?:/.*)?$";
        }
        
        patterns.emplace_back(re_pattern, negated, dir_only);
    }
    return patterns;
}

// Gitignore matcher that handles nested .gitignore files correctly.
// Also honors .gogolignore (same syntax, applied ON TOP of .gitignore at every
// level: global, repo-root, and nested per-directory). .gogolignore lets a user
// exclude paths from gogol indexing without polluting the repo's .gitignore —
// useful for vendored/autogenerated trees that ARE tracked by git (so .gitignore
// can't touch them) but shouldn't be embedded.
class GitignoreMatcher {
public:
    explicit GitignoreMatcher(const fs::path& repo_root) : repo_root_(repo_root) {
        std::error_code ec;
        
        // 1. Global excludes: ~/.config/git/ignore or core.excludesFile,
        //    plus the gogol siblings ~/.config/gogol/ignore / ~/.gogolignore_global.
        if (const char* home = std::getenv("HOME")) {
            auto global = fs::path(home) / ".config" / "git" / "ignore";
            if (fs::exists(global, ec)) {
                load_patterns(global, "");
            } else {
                global = fs::path(home) / ".gitignore_global";
                if (fs::exists(global, ec)) {
                    load_patterns(global, "");
                }
            }
            auto gglobal = fs::path(home) / ".config" / "gogol" / "ignore";
            if (fs::exists(gglobal, ec)) {
                load_patterns(gglobal, "");
            } else {
                gglobal = fs::path(home) / ".gogolignore_global";
                if (fs::exists(gglobal, ec)) {
                    load_patterns(gglobal, "");
                }
            }
        }
        
        // 2. Repo-level excludes: .git/info/exclude (git only — gogol has no
        //    .git/info equivalent by design; .gogolignore is a repo-root file).
        if (!repo_root.empty()) {
            auto exclude = repo_root / ".git" / "info" / "exclude";
            if (fs::exists(exclude, ec)) {
                load_patterns(exclude, "");
            }
        }
        
        // 3. Root .gitignore, then root .gogolignore (on top, same layer)
        if (!repo_root.empty()) {
            auto root_gi = repo_root / ".gitignore";
            if (fs::exists(root_gi, ec)) {
                load_patterns(root_gi, "");
            }
            auto root_gi_g = repo_root / ".gogolignore";
            if (fs::exists(root_gi_g, ec)) {
                load_patterns(root_gi_g, "");
            }
        }
    }
    
    // Load .gitignore + .gogolignore from a directory (called during traversal).
    // Both use the same parser; .gogolignore is loaded after .gitignore so its
    // patterns apply on top (later patterns win in is_ignored's ordered scan).
    void enter_directory(const fs::path& dir) {
        std::error_code ec;
        std::string rel = fs::relative(dir, repo_root_, ec).string();
        if (ec || rel == ".") rel = "";
        std::vector<IgnorePattern> merged;
        for (const char *name : {".gitignore", ".gogolignore"}) {
            auto gi = dir / name;
            if (fs::exists(gi, ec)) {
                auto patterns = parse_gitignore(gi, rel);
                for (auto &p : patterns) merged.push_back(std::move(p));
            }
        }
        if (!merged.empty()) {
            dir_patterns_.push_back({dir.string(), std::move(merged)});
        }
    }
    
    // Remove patterns when leaving a directory
    void leave_directory(const fs::path& dir) {
        while (!dir_patterns_.empty() && dir_patterns_.back().first == dir.string()) {
            dir_patterns_.pop_back();
        }
    }
    
    // Check if a path should be ignored.
    bool is_ignored(const std::string& rel_path, bool is_dir) const {
        // .git is always ignored
        if (rel_path == ".git" || rel_path.find(".git/") == 0 || 
            rel_path.find("/.git") != std::string::npos) {
            return true;
        }
        
        bool ignored = false;
        
        // Check global + root patterns
        for (const auto& pat : base_patterns_) {
            if (!pat.valid) continue;
            if (pat.dir_only && !is_dir) continue;
            
            try {
                if (std::regex_search(rel_path, pat.regex)) {
                    ignored = !pat.negated;
                }
            } catch (...) {}
        }
        
        // Check directory-level patterns (in order, deeper = later = higher priority)
        for (const auto& [dir, patterns] : dir_patterns_) {
            for (const auto& pat : patterns) {
                if (!pat.valid) continue;
                if (pat.dir_only && !is_dir) continue;
                
                try {
                    if (std::regex_search(rel_path, pat.regex)) {
                        ignored = !pat.negated;
                    }
                } catch (...) {}
            }
        }
        
        return ignored;
    }

private:
    void load_patterns(const fs::path& file, const std::string& base) {
        auto patterns = parse_gitignore(file, base);
        for (auto& p : patterns) {
            base_patterns_.push_back(std::move(p));
        }
    }
    
    fs::path repo_root_;
    std::vector<IgnorePattern> base_patterns_;
    std::vector<std::pair<std::string, std::vector<IgnorePattern>>> dir_patterns_;
};

bool is_nested_repo_boundary(const fs::path &dir, const fs::path &repo_root) {
    if (repo_root.empty()) return false;
    std::error_code ec;
    fs::path cdir = fs::weakly_canonical(dir, ec);
    if (ec) cdir = dir;
    fs::path croot = fs::weakly_canonical(repo_root, ec);
    if (ec) croot = repo_root;
    if (cdir == croot) return false;
    return fs::exists(dir / ".git", ec);
}

std::string git_index_name(const fs::path &toplevel) {
    if (toplevel.empty()) return "";
    fs::path p = toplevel;
    std::string s = p.generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return fs::path(s).filename().string();
}

std::string repo_tag(const std::string &idx) {
    if (idx.empty()) return "";
    fs::path p(idx);
    if (p.filename() == ".git") p = p.parent_path();
    std::string name = p.filename().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".git")
        name = name.substr(0, name.size() - 4);
    return name;
}

bool path_is_ignored(const std::filesystem::path &abs_path,
                     const std::filesystem::path &index_root) {
    // INVARIANT §4 (docs/INVARIANTS.md): the ONE ignore predicate every surface must
    // reuse (scan + sg) — never a bespoke skip list (bug ea18f2e). "ignored" == "not in
    // the index" == "deleted" (bug 112b78a).
    std::error_code ec;
    GitignoreMatcher matcher(index_root);
    fs::path rel = fs::relative(abs_path, index_root, ec);
    if (ec) return false;
    bool is_dir = fs::is_directory(abs_path, ec);
    return matcher.is_ignored(rel.string(), is_dir);
}

// Fast change detection: mtime + size combined into one hash
static uint64_t stat_hash(const fs::directory_entry& entry) {
    std::error_code ec;
    auto ftime = entry.last_write_time(ec).time_since_epoch().count();
    auto size = entry.file_size(ec);
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= static_cast<uint64_t>(ftime);
    h *= 0x100000001b3ULL;
    h ^= static_cast<uint64_t>(size);
    h *= 0x100000001b3ULL;
    return h;
}

std::vector<std::pair<std::string, uint64_t>> scan_md_files(const fs::path& root,
    const std::vector<std::string>& extensions,
    const fs::path& repo_root) {
    std::vector<std::pair<std::string, uint64_t>> results;
    std::error_code ec;
    fs::path canon_root = fs::canonical(root, ec);
    if (ec) canon_root = root;
    
    fs::path effective_repo = repo_root.empty() ? canon_root : repo_root;
    GitignoreMatcher matcher(effective_repo);

    // We need to manually recurse to handle enter/leave directory for nested .gitignore
    std::function<void(const fs::path&)> scan_dir = [&](const fs::path& dir) {
        std::error_code ec2;
        
        // Load .gitignore for this directory
        matcher.enter_directory(dir);
        
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec2);
             it != fs::directory_iterator(); it.increment(ec2)) {
            
            std::string rel_path = fs::relative(it->path(), canon_root, ec2).string();
            if (ec2) continue;
            
            // Skip symlinks entirely (could cause cycles or include external files)
            if (it->is_symlink()) continue;
            
            if (it->is_directory(ec2)) {
                std::string dir_name = it->path().filename().string();
                
                // Always skip .git
                if (dir_name == ".git") continue;
                
                // Skip if ignored
                if (matcher.is_ignored(rel_path, true)) continue;
                
                // Skip nested repo boundaries (only when repo_root explicitly provided)
                if (!repo_root.empty() && is_nested_repo_boundary(it->path(), repo_root)) continue;
                
                // Recurse
                scan_dir(it->path());
            }
            else if (it->is_regular_file(ec2)) {
                // Skip if ignored
                if (matcher.is_ignored(rel_path, false)) continue;
                
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                // Skip generated files
                std::string filename = it->path().filename().string();
                if (filename.find(".generated.") != std::string::npos ||
                    filename.find(".gen.") != std::string::npos ||
                    rel_path.find("__generated__") != std::string::npos) {
                    continue;
                }
                
                for (const auto& e : extensions) {
                    if (ext == e) {
                        results.emplace_back(rel_path, stat_hash(*it));
                        break;
                    }
                }
            }
        }
        
        matcher.leave_directory(dir);
    };
    
    scan_dir(canon_root);
    return results;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
