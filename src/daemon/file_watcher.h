// file_watcher.h — Filesystem watcher for auto-reindex
#pragma once
#include <string>
#include <vector>

// Start watching all configured index paths for changes.
// Calls reindex_callback with the index name when files change (debounced).
// Returns false if watching not available or disabled.
using WatchCallback = void (*)(const std::string &index_name, void *ctx);

bool start_file_watcher(int debounce_ms, WatchCallback callback, void *ctx);
void stop_file_watcher();
