#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct MemEntry {
    std::string content;
    std::vector<std::string> sources; // URIs: rel://path or mem://topic
    int64_t timestamp;                // unix seconds when created/updated
};

// Read-only: writes go through SqliteBackend.
