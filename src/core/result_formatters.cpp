#include "core/result_formatters.h"

namespace gogol {

gogol::Json RecallResult::to_json() const {
    gogol::Json obj = gogol::Json::object();
    
    // Convert results vector to JSON array
    gogol::Json results_arr = gogol::Json::array();
    for (const auto& r : results) {
        results_arr.push_back(r);
    }
    obj["results"] = results_arr;
    obj["count"] = count;
    obj["max_cosine"] = max_cosine;
    if (low_confidence) {
        obj["low_confidence"] = true;
    }
    
    return obj;
}

std::string RecallResult::to_string() const {
    return to_json().dump();
}

// Field separator for enriched embed text: ␞ (U+241E). Mirrors kFieldSep in
// file_adapter.h (duplicated here to avoid pulling the whole adapter header into the
// formatter — one constant, trivially kept in sync).
static constexpr const char* kEnrichSep = "\xE2\x90\x9E";

// Return the human-facing content: the middle {content} field of an enriched blob
// {context}␞{content}␞{keywords}. If the first separator is absent, the stored text is
// already plain (a note, or legacy) — return it unchanged.
std::string display_content(std::string_view stored) {
    auto first = stored.find(kEnrichSep);
    if (first == std::string_view::npos) return std::string(stored);
    auto content_start = first + std::char_traits<char>::length(kEnrichSep);
    auto second = stored.find(kEnrichSep, content_start);
    auto len = (second == std::string_view::npos) ? std::string_view::npos
                                                   : second - content_start;
    return std::string(stored.substr(content_start, len));
}

// Extract snippet from enriched content (context␞content␞keywords)
// Follows gogol's convention: non-throwing, explicit error handling
static std::string extract_snippet(std::string_view content) {
    std::string real = display_content(content);
    if (real.empty()) return "(no content)";

    std::string snip = real.substr(0, 400);
    if (snip.size() == 400) {
        auto sp = snip.rfind(' ');
        if (sp != std::string::npos && sp > 200) {
            snip = snip.substr(0, sp);
        }
        snip += "...";
    }
    return snip;
}

RecallResult format_recall_results(
    const std::vector<store::Store::HybridHit>& hits,
    store::Store& store,
    float global_max_cosine
) {
    RecallResult result;
    result.count = 0;
    result.max_cosine = global_max_cosine;
    result.low_confidence = global_max_cosine < 0.7f;
    
    for (const auto& h : hits) {
        auto e = store.get(h.id);
        if (!e) continue;
        
        gogol::Json hit_obj = gogol::Json::object();
        hit_obj["id"] = h.id;
        hit_obj["score"] = h.score;
        
        if (h.max_cosine > 0.0f) {
            hit_obj["cosine"] = h.max_cosine;
        }
        
        if (!e->symbol.empty()) {
            hit_obj["symbol"] = e->symbol;
        }
        
        std::string snip = extract_snippet(e->content);
        hit_obj["snippet"] = snip;
        
        std::string disp_index;
        if (!e->indexes.empty()) {
            disp_index = store.index_display_path(e->indexes[0]);
            hit_obj["index"] = disp_index;
            if (disp_index != e->indexes[0]) hit_obj["index_id"] = e->indexes[0];
        }
        
        hit_obj["type"] = e->type;
        // source is stored REPO-RELATIVE; reconstruct the absolute path at the resolved
        // checkout so a clone/worktree/moved repo shows ITS path (store::display_source).
        hit_obj["source"] = e->indexes.empty() ? e->source
                                               : store.display_source(e->indexes[0], e->source);
        if (e->line > 0) {
            hit_obj["line"] = static_cast<long>(e->line);
        }
        if (e->end_line > 0) {
            hit_obj["end_line"] = static_cast<long>(e->end_line);
        }
        
        hit_obj["via_edge"] = h.via_edge;
        
        if (e->type == "code") {
            auto em = store.metric_for_entry(h.id);
            if (em.found) {
                gogol::Json metric_obj = gogol::Json::object();
                
                if (em.is_exact) {
                    metric_obj["fn"] = em.m.name;
                    metric_obj["fn_complexity"] = static_cast<long>(em.m.complexity);
                    metric_obj["fn_lines"] = static_cast<long>(em.m.lines);
                    metric_obj["max_depth"] = static_cast<long>(em.m.max_depth);
                    
                    auto lc = store.link_counts(store::symbol_tail(em.m.name));
                    metric_obj["callers"] = static_cast<long>(lc.callers);
                    metric_obj["callees"] = static_cast<long>(lc.callees);
                } else {
                    metric_obj["file_max_complexity"] = static_cast<long>(em.m.complexity);
                    metric_obj["worst_fn"] = em.m.name;
                    metric_obj["worst_fn_lines"] = static_cast<long>(em.m.lines);
                }
                
                hit_obj["metric"] = metric_obj;
            }
        }
        
        result.results.push_back(hit_obj);
        result.count++;
    }
    
    return result;
}

}  // namespace gogol
