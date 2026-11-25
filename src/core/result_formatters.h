#pragma once

#include <string>
#include <vector>
#include "store/store.h"
#include "mcp/json.h"

namespace gogol {

// Unified result formatting for both MCP and CLI output.
// All formatters return JSON that can be used by either transport layer.
// Follows modern-cpp skill: const vector& for non-owning views, [[nodiscard]] 
// for functions where ignoring output is a bug.

// Recall (semantic hybrid search) results
struct RecallResult {
    std::vector<gogol::Json> results;  // array of hit objects
    long count = 0;
    float max_cosine = 0.0f;
    bool low_confidence = false;
    
    [[nodiscard]] gogol::Json to_json() const;
    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]]
RecallResult format_recall_results(
    const std::vector<store::Store::HybridHit>& hits,
    store::Store& store,
    float global_max_cosine
);

// Stored code/doc `content` is the ENRICHED embed text: {context}␞{content}␞{keywords}
// (␞ = U+241E record separator). That form is what gets embedded + FTS-indexed, but the
// context/keywords fields are embedder scaffolding, not for humans. For any DISPLAY path
// (get, recall snippets, the status page), show only the real middle field. Returns the
// input unchanged when no separator is present (notes, or pre-enrichment content).
[[nodiscard]] std::string display_content(std::string_view stored);

// Delve results (symbol + definition + structure)
struct DelveResult {
    gogol::Json content;  // structured data (symbol, definition, callers, callees)
    
    [[nodiscard]] gogol::Json to_json() const { return content; }
    [[nodiscard]] std::string to_string() const { return content.dump(); }
};

// Get/Related results (entries with metadata)
struct EntryListResult {
    std::vector<gogol::Json> results;
    long count = 0;
    
    [[nodiscard]] gogol::Json to_json() const {
        gogol::Json obj = gogol::Json::object();
        gogol::Json results_arr = gogol::Json::array();
        for (const auto& r : results) {
            results_arr.push_back(r);
        }
        obj["results"] = results_arr;
        obj["count"] = count;
        return obj;
    }
    
    [[nodiscard]] std::string to_string() const {
        return to_json().dump();
    }
};

// Metrics results (functions ranked by complexity)
struct MetricsResult {
    std::vector<gogol::Json> results;
    long count = 0;
    
    [[nodiscard]] gogol::Json to_json() const {
        gogol::Json obj = gogol::Json::object();
        gogol::Json results_arr = gogol::Json::array();
        for (const auto& r : results) {
            results_arr.push_back(r);
        }
        obj["results"] = results_arr;
        obj["count"] = count;
        return obj;
    }
    
    [[nodiscard]] std::string to_string() const {
        return to_json().dump();
    }
};

// Links results (graph navigation)
struct LinksResult {
    std::vector<gogol::Json> results;
    long count = 0;
    
    [[nodiscard]] gogol::Json to_json() const {
        gogol::Json obj = gogol::Json::object();
        gogol::Json results_arr = gogol::Json::array();
        for (const auto& r : results) {
            results_arr.push_back(r);
        }
        obj["results"] = results_arr;
        obj["count"] = count;
        return obj;
    }
    
    [[nodiscard]] std::string to_string() const {
        return to_json().dump();
    }
};

// Literal search results
struct LiteralResult {
    std::vector<gogol::Json> results;
    long count = 0;
    
    [[nodiscard]] gogol::Json to_json() const {
        gogol::Json obj = gogol::Json::object();
        gogol::Json results_arr = gogol::Json::array();
        for (const auto& r : results) {
            results_arr.push_back(r);
        }
        obj["results"] = results_arr;
        obj["count"] = count;
        return obj;
    }
    
    [[nodiscard]] std::string to_string() const {
        return to_json().dump();
    }
};

// Status results (store statistics)
struct StatusResult {
    gogol::Json content;
    
    [[nodiscard]] gogol::Json to_json() const { return content; }
    [[nodiscard]] std::string to_string() const { return content.dump(); }
};


}  // namespace gogol
