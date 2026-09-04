// file_watcher.cpp — Filesystem watcher (kqueue on macOS, inotify on Linux)
#include "config/debug.h"
#include "daemon/file_watcher.h"
#include "config/config.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#define HAS_FSEVENTS 1
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <limits.h>
#define HAS_INOTIFY 1
#endif

namespace fs = std::filesystem;

static std::atomic<bool> g_watching{false};
static std::thread g_watch_thread;
static std::mutex g_mutex;
static std::map<std::string, std::chrono::steady_clock::time_point> g_pending;
static std::map<std::string, std::string> g_dir_to_index; // watched dir → index name
static int g_debounce_ms = 2000;
static WatchCallback g_callback = nullptr;
static void *g_callback_ctx = nullptr;
static std::map<std::string, std::set<std::string>> g_index_exts; // index name → watched extensions
static std::map<std::string, int> g_index_debounce; // index name → effective debounce ms

// Debounce: fire callback after events settle
static void debounce_loop() {
  while (g_watching.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<std::string> ready;
    auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (auto it = g_pending.begin(); it != g_pending.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - it->second).count();
        int debounce = g_debounce_ms;
        auto dit = g_index_debounce.find(it->first);
        if (dit != g_index_debounce.end()) debounce = dit->second;
        if (elapsed >= debounce) {
          ready.push_back(it->first);
          it = g_pending.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto &idx : ready) {
      DBG("watch: triggering reindex for %s", idx.c_str()); if (g_callback) g_callback(idx, g_callback_ctx);
    }
  }
}

static void mark_changed(const std::string &dir, const std::string &filename = "") {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_dir_to_index.find(dir);
  if (it == g_dir_to_index.end()) return;
  const auto &idx_name = it->second;

  // Filter by extension if filename provided
  if (!filename.empty() && g_index_exts.count(idx_name)) {
    // Skip hidden files
    if (filename[0] == '.') return;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
      std::string ext = filename.substr(dot + 1);
      if (!g_index_exts[idx_name].count(ext)) return;
    } else {
      return; // no extension, skip
    }
  }

  g_pending[idx_name] = std::chrono::steady_clock::now();
}


#ifdef HAS_FSEVENTS
// macOS: use FSEvents for recursive directory watching
static void fsevents_callback(ConstFSEventStreamRef stream, void *ctx,
                              size_t count, void *paths,
                              const FSEventStreamEventFlags flags[],
                              const FSEventStreamEventId ids[]) {
  (void)stream; (void)ctx; (void)ids;
  char **cpaths = (char **)paths;
  for (size_t i = 0; i < count; i++) {
    // Skip events that aren't real content changes
    if (flags[i] & (kFSEventStreamEventFlagItemIsDir |
                    kFSEventStreamEventFlagItemIsSymlink))
      continue;
    if (!(flags[i] & (kFSEventStreamEventFlagItemModified |
                      kFSEventStreamEventFlagItemCreated |
                      kFSEventStreamEventFlagItemRemoved |
                      kFSEventStreamEventFlagItemRenamed)))
      continue;

    std::string changed_path(cpaths[i]);

    // Skip hidden files/dirs (.git, .DS_Store, etc.)
    if (changed_path.find("/.") != std::string::npos)
      continue;

    // Check extension matches configured index
    std::string ext;
    auto dot = changed_path.rfind('.');
    if (dot != std::string::npos)
      ext = changed_path.substr(dot + 1);

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[dir, idx_name] : g_dir_to_index) {
      if (changed_path.find(dir) == 0) {
        // Only trigger if extension matches
        if (!ext.empty() && g_index_exts.count(idx_name) &&
            !g_index_exts[idx_name].count(ext))
          break;
        g_pending[idx_name] = std::chrono::steady_clock::now();
        DBG("watch: change in %s (%s)", idx_name.c_str(), changed_path.c_str());
        break;
      }
    }
  }
}

static FSEventStreamRef g_stream = nullptr;
static CFRunLoopRef g_runloop = nullptr;

static void watch_loop(std::vector<std::string> dirs) {
  CFMutableArrayRef pathsToWatch = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  for (auto &dir : dirs) {
    CFStringRef path = CFStringCreateWithCString(NULL, dir.c_str(), kCFStringEncodingUTF8);
    CFArrayAppendValue(pathsToWatch, path);
    CFRelease(path);
  }

  FSEventStreamContext ctx = {0, nullptr, nullptr, nullptr, nullptr};
  g_stream = FSEventStreamCreate(NULL, fsevents_callback, &ctx,
                                  pathsToWatch, kFSEventStreamEventIdSinceNow,
                                  1.0, // latency in seconds (coalesces events)
                                  kFSEventStreamCreateFlagFileEvents |
                                  kFSEventStreamCreateFlagNoDefer);
  CFRelease(pathsToWatch);

  g_runloop = CFRunLoopGetCurrent();
  FSEventStreamScheduleWithRunLoop(g_stream, g_runloop, kCFRunLoopDefaultMode);
  FSEventStreamStart(g_stream);

  // Run until stopped
  while (g_watching.load()) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
  }

  FSEventStreamStop(g_stream);
  FSEventStreamInvalidate(g_stream);
  FSEventStreamRelease(g_stream);
  g_stream = nullptr;
}
#endif

#ifdef HAS_INOTIFY
// Linux: use inotify
static void watch_loop(std::vector<std::string> dirs) {
  int ifd = inotify_init1(IN_NONBLOCK);
  if (ifd < 0) return;

  std::map<int, std::string> wd_to_dir;
  for (auto &dir : dirs) {
    int wd = inotify_add_watch(ifd, dir.c_str(),
                               IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE);
    if (wd >= 0) wd_to_dir[wd] = dir;

    // Also watch subdirectories (one level for now)
    try {
      for (auto &entry : fs::directory_iterator(dir)) {
        if (entry.is_directory() && entry.path().filename().string()[0] != '.') {
          int swd = inotify_add_watch(ifd, entry.path().c_str(),
                                      IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE);
          if (swd >= 0) wd_to_dir[swd] = dir; // map back to parent index
        }
      }
    } catch (...) {}
  }

  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

  while (g_watching.load()) {
    int len = read(ifd, buf, sizeof(buf));
    if (len > 0) {
      char *ptr = buf;
      while (ptr < buf + len) {
        auto *event = (struct inotify_event *)ptr;
        auto it = wd_to_dir.find(event->wd);
        if (it != wd_to_dir.end()) {
          std::string name = (event->len > 0) ? event->name : "";
          mark_changed(it->second, name);
        }
        ptr += sizeof(struct inotify_event) + event->len;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  close(ifd);
}
#endif

bool start_file_watcher(int debounce_ms, WatchCallback callback, void *ctx) {
#if !defined(HAS_FSEVENTS) && !defined(HAS_INOTIFY)
  (void)debounce_ms; (void)callback; (void)ctx;
  return false;
#else
  auto configs = load_config();
  if (configs.empty()) return false;
  auto gc = load_global_config();

  g_debounce_ms = debounce_ms;
  g_callback = callback;
  g_callback_ctx = ctx;

  std::vector<std::string> dirs;
  for (auto &[name, cfg] : configs) {
    if (!cfg.is_indexed() || cfg.paths.empty()) continue;
    // Honor the per-index watch override (global default unless the index
    // sets its own `watch =`). Indexes with watch off are not scheduled.
    if (!effective_watch(cfg, gc)) continue;
    // Record this index's effective debounce (per-index override or global).
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_index_debounce[name] = effective_watch_debounce_ms(cfg, gc);
    }
    for (auto &p : cfg.paths) {
      if (fs::exists(p)) {
        std::string canonical = fs::canonical(p).string();
        dirs.push_back(canonical);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dir_to_index[canonical] = name;
      }
    }
    // Store configured extensions for filtering (strip leading dots)
    if (!cfg.extensions.empty()) {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (auto &e : cfg.extensions) {
        std::string ext = (e.size() > 1 && e[0] == '.') ? e.substr(1) : e;
        g_index_exts[name].insert(ext);
      }
    }
  }

  if (dirs.empty()) return false;

  g_watching.store(true);
  g_watch_thread = std::thread([dirs]() {
    std::thread debounce(debounce_loop);
    watch_loop(dirs);
    debounce.join();
  });

  return true;
#endif
}

void stop_file_watcher() {
  g_watching.store(false);
  if (g_watch_thread.joinable())
    g_watch_thread.join();
}
