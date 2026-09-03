#include "config/scanner.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static const std::vector<std::string> DEFAULT_SKIP_DIRS = {
    ".git"
};

static std::vector<std::string> load_ignore_patterns(const fs::path& root) {
    std::vector<std::string> patterns = DEFAULT_SKIP_DIRS;

    // Load patterns from a file
    auto load_file = [&](const fs::path& path) {
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            patterns.push_back(line);
        }
    };

    // Global: ~/.gogolignore
    if (const char* home = std::getenv("HOME")) {
        load_file(fs::path(home) / ".gogolignore");
    }
    // Local: {scan_root}/.gogolignore
    load_file(root / ".gogolignore");

    return patterns;
}

static bool should_skip_dir(const std::string& name, const std::vector<std::string>& patterns) {
    for (auto& skip : patterns) {
        if (name == skip) return true;
    }
    return false;
}

// FNV-1a 64-bit
static uint64_t fnv1a_64(const char* data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Fast change detection: mtime + size combined into one hash
static uint64_t stat_hash(const fs::directory_entry& entry) {
    auto ftime = entry.last_write_time().time_since_epoch().count();
    auto size = entry.file_size();
    // Combine mtime and size into a single hash
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= static_cast<uint64_t>(ftime);
    h *= 0x100000001b3ULL;
    h ^= static_cast<uint64_t>(size);
    h *= 0x100000001b3ULL;
    return h;
}

std::vector<std::pair<std::string, uint64_t>> scan_md_files(const fs::path& root,
    const std::vector<std::string>& extensions) {
    std::vector<std::pair<std::string, uint64_t>> results;
    std::error_code ec;
    fs::path canon_root = fs::canonical(root);
    auto patterns = load_ignore_patterns(canon_root);

    auto it = fs::recursive_directory_iterator(canon_root,
        fs::directory_options::skip_permission_denied, ec);
    auto end = fs::recursive_directory_iterator();

    while (it != end) {
        if (it->is_directory()) {
            if (should_skip_dir(it->path().filename().string(), patterns)) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }
        if (it->is_regular_file()) {
            // Skip symlinks
            if (it->is_symlink()) {
                it.increment(ec);
                continue;
            }
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            // Skip generated files
            std::string filename = it->path().filename().string();
            std::string rel_path = fs::relative(it->path(), canon_root).string();
            if (filename.find(".generated.") != std::string::npos ||
                filename.find(".gen.") != std::string::npos ||
                rel_path.find("__generated__") != std::string::npos) {
                it.increment(ec);
                continue;
            }

            for (auto& e : extensions) {
                if (ext == e) {
                    results.emplace_back(rel_path, stat_hash(*it));
                    break;
                }
            }
        }
        it.increment(ec);
    }
    return results;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
