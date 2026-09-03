// call_graph_query.cpp — Shared call graph query helpers
#include "core/call_graph_query.h"
#include "adapters/file_adapter.h"
#include "storage/index_file.h"
#include "storage/storage_backend.h"
#include <set>

void CallGraphQuery::load(const std::vector<std::string>& index_names, bool verbose) {
    configs = load_config();
    for (auto& idx_name : index_names) {
        auto backend = open_backend(idx_name);
        graphs.push_back(backend->load_call_graph());
        graph_names.push_back(idx_name);
        if (verbose) mstores[idx_name] = backend->load_metrics();
    }
    // Preload hashes for staleness
    for (auto& idx_name : index_names) {
        if (!configs.count(idx_name)) continue;
        auto idx = open_backend(idx_name)->load_index();
        for (auto& e : idx.entries)
            if (e.proto == EntryType::DOC)
                idx_hashes[idx_name][e.path] = e.hash;
    }
}

std::vector<Edge> CallGraphQuery::find_callees(const std::string& name) {
    std::vector<Edge> result; std::set<std::string> seen;
    for (size_t i = 0; i < graphs.size(); i++) {
        for (auto& e : graphs[i].callees_of(name)) {
            std::string key = e.callee + "\t" + e.file;
            if (seen.count(key)) continue;
            seen.insert(key);
            result.push_back({e.callee, e.file, graph_names[i], e.line});
        }
    }
    return result;
}

std::vector<Edge> CallGraphQuery::find_callees_in(const std::string& name, const std::string& file) {
    std::vector<Edge> result; std::set<std::string> seen;
    for (size_t i = 0; i < graphs.size(); i++) {
        for (auto& e : graphs[i].callees_of(name, file)) {
            std::string key = e.callee + "\t" + e.file;
            if (seen.count(key)) continue;
            seen.insert(key);
            result.push_back({e.callee, e.file, graph_names[i], e.line});
        }
    }
    return result;
}

std::vector<Edge> CallGraphQuery::find_callers(const std::string& name) {
    std::vector<Edge> result; std::set<std::string> seen;
    for (size_t i = 0; i < graphs.size(); i++) {
        for (auto& e : graphs[i].callers_of(name)) {
            std::string key = e.caller + "\t" + e.file;
            if (seen.count(key)) continue;
            seen.insert(key);
            result.push_back({e.caller, e.file, graph_names[i], e.line});
        }
    }
    return result;
}

std::vector<std::string> CallGraphQuery::defining_files(const std::string& name) {
    std::vector<std::string> files;
    for (size_t i = 0; i < graphs.size(); i++)
        for (auto& f : graphs[i].files_defining(name))
            files.push_back(f);
    return files;
}

std::string CallGraphQuery::find_index_for(const std::string& func_name, const std::string& file) {
    for (size_t i = 0; i < graphs.size(); i++) {
        for (auto& e : graphs[i].edges) {
            if (e.caller == func_name && e.file == file)
                return graph_names[i];
        }
    }
    return "";
}

std::string CallGraphQuery::get_status(const std::string& idx_name, const std::string& file) {
    std::string key = idx_name + ":" + file;
    auto it = stale_cache.find(key);
    if (it != stale_cache.end()) return it->second;
    if (!configs.count(idx_name)) { stale_cache[key] = "ok"; return "ok"; }
    uint64_t hash = 0;
    if (idx_hashes.count(idx_name) && idx_hashes[idx_name].count(file))
        hash = idx_hashes[idx_name][file];
    FileAdapter fa(idx_name, configs[idx_name]);
    auto st = fa.check_stale(file, hash);
    std::string s = "ok";
    if (st == EntryStatus::MISSING) s = "missing";
    else if (st == EntryStatus::STALE) s = "stale";
    stale_cache[key] = s;
    return s;
}
