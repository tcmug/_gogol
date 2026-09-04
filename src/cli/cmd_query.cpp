// cmd_query.cpp — query and list commands
#include "cli/commands.h"
#include "config/config.h"
#include "config/utils.h"
#include "core/format.h"
#include "core/loc.h"
#include "core/searcher.h"
#include "daemon/embed_client.h"
#include "daemon/embed_server.h"
#include "storage/docref_store.h"
#include "storage/storage_backend.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

using std::cerr; using std::cout; using std::map;
using std::string; using std::vector;

// --- cmd_query ---

int cmd_query(const Args& args) {
    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;

    QueryRequest req;
    req.query = args.query_text;
    req.index = args.name;
    req.type_filter = args.has_type ? (int)args.entry_type : -1;
    req.top_k = args.top_k;
    req.show_lines = args.show_lines;
    req.scores = args.scores;
    vector<QueryResultEntry> rpc_results;
    string err = rpc.cmd_query(req, rpc_results);
    if (!err.empty()) { cerr << err << "\n"; return 1; }
    auto configs = load_config();
    // Per-index doc-reference graph cache (loaded lazily, once per index).
    map<string, DocRefGraph> docref_cache;
    for (auto& r : rpc_results) {
        SearchResult sr;
        sr.index = r.index; sr.path = r.path; sr.line = r.line;
        sr.chunk = r.chunk; sr.proto = r.proto; sr.status = r.status;
        sr.score = r.score; sr.cosine = r.cosine; sr.snippet = r.snippet;

        string extra;
        if (sr.proto == 0) { // doc entries only
            auto it = docref_cache.find(sr.index);
            if (it == docref_cache.end())
                it = docref_cache.emplace(sr.index, open_backend(sr.index)->load_docref_graph()).first;
            const DocRefGraph& g = it->second;

            int refs = 0, ext = 0;
            for (const auto& e : g.references_of(sr.path)) {
                if (e.kind == RefKind::Local) refs++;
                else if (e.kind == RefKind::External) ext++;
            }
            int refby = (int)g.referenced_by(sr.path).size();

            if (refs > 0 || ext > 0 || refby > 0) {
                string body;
                auto add = [&](const char* label, int n) {
                    if (n <= 0) return;
                    if (!body.empty()) body += " ";
                    body += label; body += ":"; body += std::to_string(n);
                };
                add("refs", refs); add("ext", ext); add("refby", refby);
                if (g_format == OutputFormat::AGENT)
                    extra = " " + body;
                else
                    extra = "   [" + body + "]";
            }
        }
        print_result(sr, args.scores, configs, extra);
    }
    return 0;
}

// --- cmd_list ---

int cmd_list(const Args& args) {
    // No index given: list the available indexes (discovery view). This mirrors
    // query/explore/calls treating "no index" as "operate across all indexes",
    // and answers "what can I list/query?". `gogol status` remains the daemon /
    // health / indexing-progress diagnostic; `list` is the entry-point summary.
    if (args.name.empty()) {
        auto configs = load_config();
        for (auto& [cfg_name, cfg] : configs) {
            if (!cfg.is_indexed()) continue;
            auto backend = open_backend(cfg_name);
            auto c = backend->load_index_counts();
            int term_count = (int)backend->load_glossary().size();
            if (g_format == OutputFormat::AGENT)
                printf("%s\t%d\t%d\t%d\n", cfg_name.c_str(),
                       c.file_count, c.mem_count, term_count);
            else
                printf("%-14s %5d doc %4d note %4d term\n", cfg_name.c_str(),
                       c.file_count, c.mem_count, term_count);
        }
        return 0;
    }

    RpcClient rpc;
    if (!require_daemon(rpc)) return 1;

    vector<ListResultEntry> entries;
    string err = rpc.cmd_list(args.entry_type, args.has_type, args.name, entries);
    if (!err.empty()) { cerr << err << "\n"; return 1; }
    auto configs = load_config();
    for (auto& e : entries) {
        if (e.proto == 1) // MEM (note)
            printf("%s\n", format_loc(EntryType::NOTE, e.index, e.path).c_str());
        else if (e.proto == 2) // GLOSSARY (term)
            printf("%s\n", format_loc(EntryType::TERM, e.index, e.path).c_str());
        else { // FILE (doc)
            printf("%s\n", format_doc_ref(e.index, e.path, e.line, e.chunk, configs).c_str());
        }
    }
    return 0;
}
