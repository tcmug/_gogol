// call_graph_query.h — Shared call graph query helpers
#pragma once
#include "storage/call_store.h"
#include "storage/metrics_store.h"
#include "config/config.h"
#include <map>
#include <string>
#include <vector>

struct Edge {
    std::string name;
    std::string file;
    std::string index;
    uint32_t line;
};

struct CallGraphQuery {
    std::vector<CallGraph> graphs;
    std::vector<std::string> graph_names;
    std::map<std::string, MetricsStore> mstores;
    std::map<std::string, IndexConfig> configs;
    std::map<std::string, std::map<std::string, uint64_t>> idx_hashes;
    std::map<std::string, std::string> stale_cache;

    void load(const std::vector<std::string>& index_names, bool verbose);
    std::vector<Edge> find_callees(const std::string& name);
    std::vector<Edge> find_callers(const std::string& name);
    std::vector<Edge> find_callees_in(const std::string& name, const std::string& file);
    std::vector<std::string> defining_files(const std::string& name);
    std::string find_index_for(const std::string& func_name, const std::string& file);
    std::string get_status(const std::string& idx_name, const std::string& file);
};
