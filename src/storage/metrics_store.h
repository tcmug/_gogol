// metrics_store.h — Per-index function metrics storage
#pragma once
#include "core/types.h"
#include <string>
#include <vector>

// Function metrics store
struct MetricsStore {
    std::vector<FunctionMetrics> entries;

    // Query: metrics for functions in a file
    std::vector<FunctionMetrics> for_file(const std::string &file) const;
    // Query: find a specific function
    FunctionMetrics *find(const std::string &name);
};
