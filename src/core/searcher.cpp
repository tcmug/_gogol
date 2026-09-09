// searcher.cpp — Search pipeline implementation
#include "core/searcher.h"
#include "core/loc.h"
#include "adapters/file_adapter.h"
#include "adapters/mem_adapter.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

using std::map;
using std::string;
using std::vector;

vector<SearchResult> search(
    const SearchOptions &opts,
    EmbedProvider &embedder,
    const map<string, const Index *> &indexes,
    const map<string, IndexConfig> &configs,
    const map<string, FtsKeywordFn> &fts_providers) {

  // Collect all entries with index name tag.
  // Caller must have loaded embeddings (ensure_embeddings) before calling —
  // search() treats the indexes as immutable and never mutates them.
  struct Tagged { string idx; const IndexEntry *e; };
  vector<Tagged> all;
  for (auto &name : opts.index_names) {
    auto it = indexes.find(name);
    if (it == indexes.end() || it->second == nullptr) continue;
    for (auto &e : it->second->entries)
      all.push_back({name, &e});
  }

  if (all.empty()) return {};

  // Type filter: -1 = all; else DOC(file) / NOTE(mem) / TERM(none searchable).
  auto matches = [&](const Tagged &t) -> bool {
    if (opts.type_filter < 0) return true;
    EntryType want = (EntryType)opts.type_filter;
    bool is_mem = (t.e->proto == EntryType::NOTE);
    if (want == EntryType::DOC) return !is_mem;
    if (want == EntryType::NOTE) return is_mem;
    // TERM entries live in the glossary store, not the searchable index.
    return false;
  };

  // Embed query
  auto qvec = embedder.embed_query(opts.query);
  if (qvec.empty()) return {};

  // Cosine ranking
  struct Scored { int idx; float cos; };
  vector<Scored> ranked;
  ranked.reserve(all.size());
  for (int i = 0; i < (int)all.size(); i++) {
    if (!matches(all[i])) continue;
    float cs = cosine_similarity(qvec, all[i].e->embedding);
    ranked.push_back({i, cs});
  }
  std::sort(ranked.begin(), ranked.end(),
            [](auto &a, auto &b) { return a.cos > b.cos; });

  // RRF: cosine contribution
  const float K = 60.0f;
  vector<float> rrf(all.size(), 0.0f);
  for (int r = 0; r < (int)ranked.size(); r++)
    rrf[ranked[r].idx] += 1.0f / (K + r + 1);

  // RRF: keyword contribution.
  // Source per index: FTS5 (SQLite-backed) via the registered provider. An
  // index without a provider contributes cosine ranking only. The RRF math
  // (1/(K + rank + 1), rank 0-based) matches the cosine contribution above.
  // The provider returns (entry_index, score) best-first where entry_index is
  // the 0-based position within that index's entries, so the offset math
  // (global index = offset + local index) is shared with the cosine pass.
  {
    int offset = 0;
    for (auto &name : opts.index_names) {
      auto it = indexes.find(name);
      if (it == indexes.end() || it->second == nullptr) continue;
      int n = (int)it->second->entries.size();

      auto apply_keyword_rank = [&](int local_index, int rank) {
        int gi = offset + local_index;
        if (gi >= 0 && gi < (int)all.size() && matches(all[gi]))
          rrf[gi] += 1.0f / (K + rank + 1);
      };

      auto fts_it = fts_providers.find(name);
      if (fts_it != fts_providers.end() && fts_it->second) {
        // SQLite-backed: keyword ranking comes from FTS5 MATCH on the DB.
        auto results = fts_it->second(opts.query, n);
        for (int r = 0; r < (int)results.size(); r++)
          apply_keyword_rank(results[r].first, r);
      }
      offset += n;
    }
  }

  // Build sorted results
  std::unordered_map<int, float> cos_map;
  for (auto &s : ranked) cos_map[s.idx] = s.cos;

  struct Candidate { float score; float cos; string idx; const IndexEntry *e; };
  vector<Candidate> candidates;
  for (int i = 0; i < (int)all.size(); i++) {
    if (!matches(all[i]) || rrf[i] == 0.0f) continue;
    candidates.push_back({rrf[i], cos_map.count(i) ? cos_map[i] : 0.0f,
                          all[i].idx, all[i].e});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](auto &a, auto &b) { return a.score > b.score; });

  int count = std::min(opts.top_k, (int)candidates.size());
  vector<SearchResult> results;
  results.reserve(count);

  for (int i = 0; i < count; i++) {
    auto &c = candidates[i];
    SearchResult sr;
    sr.score = c.score;
    sr.cosine = c.cos;
    sr.index = c.idx;
    sr.path = c.e->path;
    sr.line = c.e->line;
    sr.chunk = c.e->chunk;
    sr.proto = (uint8_t)c.e->proto;

    // Staleness
    sr.status = "ok";
    if (c.e->proto == EntryType::DOC && configs.count(c.idx)) {
      FileAdapter fa(c.idx, configs.at(c.idx));
      auto st = fa.check_stale(c.e->path, c.e->hash);
      if (st == EntryStatus::MISSING) sr.status = "missing";
      else if (st == EntryStatus::STALE) sr.status = "stale";
    } else if (c.e->proto == EntryType::NOTE) {
      MemAdapter ma(c.idx);
      auto st = ma.check_stale(c.e->path, c.e->hash);
      if (st == EntryStatus::MISSING) sr.status = "missing";
      else if (st == EntryStatus::STALE) sr.status = "stale";
    }

    // Snippet
    if (opts.show_lines > 0 && sr.status == "ok") {
      if (c.e->proto == EntryType::NOTE) {
        MemAdapter ma(c.idx);
        sr.snippet = ma.get_content(c.e->path, 0, opts.show_lines);
      } else if (configs.count(c.idx)) {
        FileAdapter fa(c.idx, configs.at(c.idx));
        sr.snippet = fa.get_content(c.e->path, c.e->line, opts.show_lines);
      }
    }

    results.push_back(std::move(sr));
  }
  return results;
}
