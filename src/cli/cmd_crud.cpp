// cmd_crud.cpp — index, add, rm, get, status, sync, prune commands
#include "cli/commands.h"
#include "config/config.h"
#include "config/utils.h"
#include "core/format.h"
#include "core/loc.h"
#include "daemon/embed_client.h"
#include "daemon/embed_server.h"
#include "storage/index_file.h"
#include "storage/mem_store.h"
#include "storage/glossary_store.h"
#include "storage/storage_backend.h"

#include <cstdlib>
#include <sstream>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <signal.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using std::cerr; using std::cout; using std::map; using std::string; using std::vector;

bool require_daemon(RpcClient& rpc) {
    if (daemon_is_running() && rpc.connected()) return true;
    cerr << "No gogol daemon running. Start it with: gogol serve\n";
    return false;
}

// --- cmd_index ---

int cmd_index(const Args& args) {
    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;
    IndexRequest req{args.name, args.force};
    cout << rpc.cmd_index(req);
    return 0;
}

// --- cmd_add ---

int cmd_add(const Args& args) {
    // Resolve content: positional arg, --stdin, or -f
    string content = args.content;
    if (args.use_stdin) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        content = ss.str();
        // Trim trailing newline
        while (!content.empty() && content.back() == '\n') content.pop_back();
    } else if (!args.content_file.empty()) {
        std::ifstream f(args.content_file);
        if (!f) { cerr << "Cannot read: " << args.content_file << "\n"; return 1; }
        std::ostringstream ss;
        ss << f.rdbuf();
        content = ss.str();
        while (!content.empty() && content.back() == '\n') content.pop_back();
    }
    if (content.empty()) {
        cerr << "No content provided. Use positional arg, --stdin, or -f <file>\n";
        return 1;
    }

    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;
    AddRequest req{args.entry_type, args.name, args.path_arg, content, args.sources};
    string result = rpc.cmd_add(req);
    cout << result << "\n";
    return result.find("Error") != string::npos ? 1 : 0;
}

// --- cmd_rm ---

int cmd_rm(const Args& args) {
    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;
    string result = rpc.cmd_rm({args.entry_type, args.name, args.path_arg});
    cout << result << "\n";
    return result.find("Error") != string::npos ? 1 : 0;
}

// --- cmd_get ---

int cmd_get(const Args& args) {
    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;
    string result = rpc.cmd_get({args.entry_type, args.name, args.path_arg}, args.get_lines);
    if (result.empty()) { cerr << "Not found\n"; return 1; }
    cout << result << "\n";
    return 0;
}

// --- cmd_status ---

int cmd_status(const Args& args) {
    auto configs = load_config();
    auto gc = load_global_config();

    // Query daemon for indexing state
    bool is_indexing = false;
    string indexing_name;
    int idx_progress = 0, idx_total = 0, idx_elapsed = 0;

    bool daemon_running = false;
    if (daemon_is_running()) {
        daemon_running = true;
        RpcClient rpc;
        if (rpc.connected()) {
            rpc.cmd_status(args.name, is_indexing, indexing_name,
                          idx_progress, idx_total, idx_elapsed);
        }
    }

    if (daemon_running) printf("daemon:  running");
    else printf("daemon:  stopped");
    if (gc.watch) printf(" (watch enabled, debounce=%dms)", gc.watch_debounce_ms);
    if (is_indexing && idx_total > 0) {
        int pct = (idx_progress * 100) / idx_total;
        // ETA: (elapsed / progress) * remaining
        int eta_sec = 0;
        if (idx_progress > 0)
            eta_sec = (idx_elapsed * (idx_total - idx_progress)) / idx_progress;
        if (eta_sec > 60)
            printf("  indexing: %s (%d%%, ~%dm %ds remaining)",
                   indexing_name.c_str(), pct, eta_sec / 60, eta_sec % 60);
        else
            printf("  indexing: %s (%d%%, ~%ds remaining)",
                   indexing_name.c_str(), pct, eta_sec);
    } else if (is_indexing) {
        printf("  indexing: %s", indexing_name.c_str());
    }
    printf("\n\n");

    // Per-index counts from header only (fast)
    for (auto& [cfg_name, cfg] : configs) {
        if (!cfg.is_indexed()) continue;
        if (!args.name.empty() && args.name != cfg_name) continue;

        auto backend = open_backend(cfg_name);
        auto c = backend->load_index_counts();
        int term_count = (int)backend->load_glossary().size();
        string health = "ok";
        if (is_indexing && indexing_name == cfg_name) {
            if (idx_total > 0) {
                int pct = (idx_progress * 100) / idx_total;
                int eta_sec = 0;
                if (idx_progress > 0)
                    eta_sec = (idx_elapsed * (idx_total - idx_progress)) / idx_progress;
                char buf[64];
                if (eta_sec > 60)
                    snprintf(buf, sizeof(buf), "indexing %d%%, ~%dm %ds", pct, eta_sec / 60, eta_sec % 60);
                else
                    snprintf(buf, sizeof(buf), "indexing %d%%, ~%ds", pct, eta_sec);
                health = buf;
            } else {
                health = "indexing";
            }
        }

        // Show per-type counts (doc / note / term) so an agent knows what an
        // index holds before querying it.
        if (g_format == OutputFormat::AGENT)
            printf("%s\t%d\t%d\t%d\t%s\n", cfg_name.c_str(),
                   c.file_count, c.mem_count, term_count, health.c_str());
        else
            printf("  %-14s %5d doc %4d note %4d term  dim=%-4d (%s)\n",
                   cfg_name.c_str(), c.file_count, c.mem_count, term_count,
                   c.dim, health.c_str());
    }
    return 0;
}

// --- cmd_sync ---

int cmd_sync(const Args& args) {
    (void)args;
    // git/w modes were removed. Memory dirs are plain directories; if you keep
    // one inside a git repo, commit/push it yourself. Nothing to sync here.
    cout << "Nothing to sync (git/export modes were removed). "
            "Memory dirs are plain files; version them in your own repo.\n";
    return 0;
}

// --- cmd_prune ---

int cmd_prune(const Args& args) {
    (void)args;
    auto configs = load_config();
    fs::path idx_dir = fs::path(std::getenv("HOME")) / ".gogol" / "indexes";
    if (!fs::exists(idx_dir)) return 0;

    for (auto& entry : fs::directory_iterator(idx_dir)) {
        string stem = entry.path().stem().string();
        if (!configs.count(stem)) {
            printf("Removing: %s\n", entry.path().c_str());
            fs::remove(entry.path());
        }
    }
    return 0;
}
