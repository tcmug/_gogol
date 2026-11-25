// refresh.cpp — on-demand refresh indexing (see refresh.h).
//
// The ingest loop here is the same scan/chunk/embed/skip-gate/reconcile machinery
// the CLI sync uses, extracted into a shared TU so the CLI, the MCP server, AND the
// unit tests all call ONE implementation. The only addition over the original in-CLI
// version is the optional per-FILE deadline: a budgeted refresh stops at a file
// boundary and leaves the remaining files unmarked so a follow-up refresh resumes.
#include "core/refresh.h"

#include "embedding/embed_provider.h"
#include "store/store.h"
#include "adapters/file_adapter.h"
#include "chunking/ts_chunker.h"        // has_treesitter_grammar, extract_*
#include "chunking/chunker_iface.h"     // default_index_extensions
#include "config/scanner.h"             // git_index_name
#include "config/config.h"              // expand_tilde
#include "cli/git_identity.h"                 // gogol::resolve_git_identity

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace gogol {

IngestResult resync_disk_index(store::Store &store, EmbedProvider &embedder,
                               const std::string &name, const IndexConfig &cfg,
                               const std::string &repo_root, Deadline deadline,
                               ProgressCallback progress, const std::string &root_sha) {
    IngestResult r;
    std::string root = cfg.paths.empty() ? "" : expand_tilde(cfg.paths[0]);
    FileAdapter fa(name, cfg);
    // git-identity indexing: skip nested-repo boundaries during scan (a subdir that
    // is a different repo is its own index). Empty repo_root => plain scan.
    if (!repo_root.empty()) fa.set_repo_root(repo_root);
    // Incremental sync: pass last-seen per-file stat-hashes so the scanner SKIPS files
    // whose mtime+size are unchanged (no re-chunk/re-embed). file_hashes is now keyed
    // by the REPO-RELATIVE path — the same key the scanner emits (ch.key) — so the
    // lookup is a direct pass-through with no abs<->rel conversion.
    auto existing_hashes = store.file_hashes(name);  // rel -> hash (this idx only)
    auto chunks = fa.scan(/*force=*/false, existing_hashes);
    auto tail = [](std::string s) { return store::symbol_tail(s); };
    std::set<std::string> edge_files_done;
    std::map<std::string, std::vector<std::string>> file_ids;   // abs -> chunk ids
    std::map<std::string, uint64_t> new_hashes;                 // abs -> stat-hash
    // Per-file extracted metrics cache (Step 5). One row per function: its decl line +
    // metrics. Populated once per file (lazily) and matched to each chunk by line range
    // so the metric attaches to ITS chunk row — the exact-attribution correctness fix.
    struct FileFnMetric { int line, complexity, lines, params, returns, max_depth; };
    std::map<std::string, std::vector<FileFnMetric>> file_metrics; // abs -> fn metrics
    const int kBatch = 500;
    int since_commit = 0;
    // Budget: check the deadline at each FILE boundary. A file's stat-hash is
    // persisted only after ALL its chunks are ingested, so stopping BEFORE starting a
    // new file leaves that (and every later) file unmarked -> the next refresh resumes
    // it. deadline == default (epoch) means "no budget".
    const bool budgeted = deadline != Deadline{};
    std::string current_abs;

    // Batched embedding: embedding one chunk per llama_encode is 3-8x slower than
    // packing several sequences into one GPU call. We buffer chunks that need a NEW
    // embedding (not the has_entry() skip path) and flush them through
    // embed_documents_batch. Side effects that depend on the resulting id
    // (store.put, map_symbol, file_ids) run when the batch flushes. kEmbedBatch
    // matches the embedder's internal n_seq_max packing (8).
    constexpr size_t kEmbedBatch = 8;
    struct Pending {
        std::string text, abs, name, sym;
        store::Store::Loc loc;
        bool is_md = false;
    };
    std::vector<Pending> pend;
    pend.reserve(kEmbedBatch);

    auto flush_embeds = [&]() {
        if (pend.empty()) return;
        std::vector<std::string> texts;
        texts.reserve(pend.size());
        for (const auto &p : pend) texts.push_back(p.text);
        auto vecs = embedder.embed_documents_batch(texts);
        for (size_t j = 0; j < pend.size(); j++) {
            const auto &p = pend[j];
            // Embedder returns one slot per input; skip a slot only if it came back
            // empty (oversized seq skipped internally).
            if (j >= vecs.size() || vecs[j].empty()) continue;
            std::string id = store.put(p.text, vecs[j], {p.name}, p.abs, p.loc, root, root_sha);
            file_ids[p.abs].push_back(id);
        }
        pend.clear();
    };

    const int total_chunks = static_cast<int>(chunks.size());
    store.begin();
    for (const auto &ch : chunks) {
        // `abs` = absolute path, used ONLY for filesystem reads (stat, open). `rel` =
        // the repo-relative path (ch.key), used for everything STORED (entries.source,
        // entry_sources, file_hashes, edges.file) so the db holds no absolute content
        // paths (R1). The absolute is reconstructed at query time from the checkout.
        std::string abs = root.empty() ? ch.key : (fs::path(root) / ch.key).string();
        const std::string &rel = ch.key;
        // File boundary: a chunk whose source differs from the one we're on. Check the
        // budget here (never mid-file) so we never persist a partially-ingested file.
        if (abs != current_abs) {
            // Flush pending embeds at the file boundary so file_ids is complete for the
            // file we're leaving (staleness purge + stat-hash below rely on file_ids).
            flush_embeds();
            if (budgeted && std::chrono::steady_clock::now() >= deadline) {
                r.hit_budget = true;
                break;
            }
            current_abs = abs;
        }
        r.read++;
        // Progress callback per-chunk (caller throttles display)
        if (progress) progress(r.read, total_chunks, abs);
        if (ch.text.empty()) continue;
        // Symbol name for the definition + the symbol->id map. Derive it from the
        // chunk HEADING (the tree-sitter chunker's real function/symbol name), NOT
        // from leading_symbol_pub(ch.text): ch.text is the ENRICHED embed text,
        // prefixed with directory-context tokens ("storage: path § name. ..."), so
        // leading_symbol_pub would return a directory token instead of the function
        // name — poisoning symbols and breaking delve/links-by-name. Falls back
        // to the enriched-text leading symbol only when the chunk has no heading.
        std::string sym = store::symbol_from_heading(ch.heading);
        if (sym.empty()) sym = store::extract_leading_symbol(ch.text);
        store::Store::Loc loc;
        loc.line = int(ch.line); loc.end_line = int(ch.end_line);
        loc.symbol = sym;
        std::string ext = fs::path(ch.key).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool is_md = ext == ".md";
        // INLINE METRICS (Step 5): attach THIS chunk's own function metric by exact
        // line match — the extracted function whose declaration line falls within the
        // chunk's [line, end_line] range. This is the correctness fix: metrics are
        // stamped onto the owning chunk row at chunk time, not fuzzily post-joined
        // (which mis-attributed ~46%). Metrics per file are computed once + cached.
        if (!is_md && has_treesitter_grammar(ext)) {
            auto it = file_metrics.find(abs);
            if (it == file_metrics.end()) {
                std::ifstream mf(abs);
                std::vector<FileFnMetric> ms;
                if (mf) {
                    std::ostringstream ss; ss << mf.rdbuf(); std::string content = ss.str();
                    if (!content.empty())
                        for (auto &fm : extract_metrics(content, ext))
                            ms.push_back({static_cast<int>(fm.line), static_cast<int>(fm.complexity), static_cast<int>(fm.lines),
                                          static_cast<int>(fm.params), static_cast<int>(fm.returns), static_cast<int>(fm.max_depth)});
                }
                it = file_metrics.emplace(abs, std::move(ms)).first;
            }
            // Pick the function whose decl line is in [chunk.line, chunk.end_line]; a
            // chunk usually holds one function — on ties, the most complex (worst).
            const int lo = loc.line, hi = loc.end_line > loc.line ? loc.end_line : loc.line;
            const FileFnMetric *best = nullptr;
            for (const auto &fm : it->second) {
                if (fm.line >= lo && fm.line <= hi &&
                    (!best || fm.complexity > best->complexity))
                    best = &fm;
            }
            if (best) {
                loc.has_metric = true;
                loc.complexity = best->complexity; loc.lines = best->lines;
                loc.params = best->params; loc.returns = best->returns;
                loc.max_depth = best->max_depth;
            }
        }
        // Embed-skip: content-hash identity means unchanged content has the same id and
        // vector. Only embed when the id is NOT already stored; else touch (no embed).
        std::string id = store::content_id(ch.text);
        if (store.has_entry(id)) {
            store.update_entry_loc(id, name, loc, root);
            file_ids[rel].push_back(id);
        } else {
            // Defer: buffer for batched embedding, flush when full. p.abs carries the
            // RELATIVE path now (stored as source); abs stays local for fs reads.
            pend.push_back({ch.text, rel, name, sym, loc, is_md});
            if (pend.size() >= kEmbedBatch) flush_embeds();
        }
        new_hashes[rel] = ch.hash;
        r.ingested++;

        // EDGES FROM DISK: call/import edges + complexity metrics per code file, once
        // per file. Refresh on every sync so the graph stays current with the source.
        if (!is_md && has_treesitter_grammar(ext) && !edge_files_done.count(ch.key)) {
            edge_files_done.insert(ch.key);
            std::ifstream f(abs);
            if (f) {
                std::ostringstream ss; ss << f.rdbuf(); std::string content = ss.str();
                if (!content.empty()) {
                    store.remove_edges_of_file(rel);
                    for (auto &e : extract_calls(content, ext))
                        store.add_edge("call", tail(e.caller), tail(e.callee), name, rel);
                    // IMPORT edges: src_sym is the IMPORTING FILE, now the RELATIVE path
                    // (consistent with entries.source, also relative). delve/affected
                    // join import-src against entry.source — both relative, so the join
                    // still resolves. dst is the raw imported module path (unchanged).
                    for (auto &imp : extract_imports(content, ext))
                        store.add_edge("import", rel, imp.module_path, name, rel);
                }
            }
        }

        if (++since_commit >= kBatch) {
            store.commit();
            store.begin();
            since_commit = 0;
        }
    }
    flush_embeds();  // embed the final partial batch (loop ends without a file boundary)
    store.commit();
    // Per-chunk staleness: for each file we just ingested, purge any OLD-content chunks
    // no longer in its current set. Unchanged chunks are kept (same id).
    store.begin();
    for (const auto &[rel, ids] : file_ids)
        r.purged += static_cast<int>(store.remove_stale_chunks(name, rel, ids));
    // Persist the stat-hash of every FULLY-ingested file (keyed by RELATIVE path) so
    // the next refresh skips it if unchanged. A budgeted stop breaks at a file
    // boundary, so new_hashes holds only complete files; unstarted ones stay unmarked.
    for (const auto &[rel, h] : new_hashes)
        store.set_file_hash(name, rel, h);
    store.commit();
    // GC deleted files: a file removed from disk produces no chunks in any scan, so
    // nothing above touches its rows. purge_missing_sources sweeps this idx's sources
    // (relative) and stats root+rel to confirm absence. The old root-prefix guard is
    // gone — a relative source is inherently scoped to its repo (queried by idx), so
    // we pass the repo_root to reconstruct absolute paths for the stat.
    {
        // `root` is already tilde-expanded above; GC stats root+rel for confirmed absence.
        r.gc_purged += static_cast<int>(store.purge_missing_sources(name, root));
    }
    return r;
}

IngestResult refresh_repo(store::Store &store, EmbedProvider &embedder,
                          const std::string &repo_toplevel,
                          const std::string &common_dir_in, Deadline deadline,
                          ProgressCallback progress) {
    IngestResult r;
    if (repo_toplevel.empty()) { r.ok = false; r.err = "empty repo toplevel"; return r; }
    std::error_code ec;
    if (!fs::exists(repo_toplevel, ec)) {
        r.ok = false; r.err = "repo toplevel does not exist: " + repo_toplevel; return r;
    }

    // Index identity (R1/T6): the repo's ROOT COMMIT SHA, a directory-agnostic,
    // move-stable, clone/worktree-shared identity. This becomes entries.idx. The SCAN
    // path stays separate (repo_toplevel / cfg.paths), so files are still read from
    // disk by path while entries are keyed by the SHA.
    std::string root_sha = gogol::resolve_root_sha(repo_toplevel).value_or("");

    // The PATH-form identity (common_dir, /.git stripped) — computed ONCE here and
    // reused as both the no-SHA fallback idx AND the re-key source. Resolve common_dir
    // from the provided arg, else via git, else the toplevel basename.
    std::string path_idx = common_dir_in;
    if (path_idx.empty()) {
        auto gid = gogol::resolve_git_identity(repo_toplevel);
        path_idx = (gid && !gid->common_dir.empty()) ? gid->common_dir
                                                     : git_index_name(repo_toplevel);
    }
    // INVARIANT §4: strip trailing /.git so a common_dir idx matches the query side.
    path_idx = gogol::strip_git_suffix(path_idx);

    // Identity = root SHA when resolvable (R1), else the path (R5, no regression for
    // empty/orphan-root repos).
    std::string idx = !root_sha.empty() ? root_sha : path_idx;
    if (idx.empty()) { r.ok = false; r.err = "could not derive index identity"; return r; }

    // Record a sighting (T2/R2): this repo's ref was seen at repo_toplevel. The RW
    // refresh handle is the natural write point. Best-effort; only when we have a SHA
    // (the location layer keys on the ref). Powers parent-dir search, move, display.
    if (!root_sha.empty())
        store.record_sighting(root_sha, repo_toplevel);

    // Synthesize the configless config: index the whole toplevel with the built-in
    // default extension set (no config `ext=` list in the git-identity model).
    IndexConfig cfg;
    cfg.name = idx;  // cfg.name is used by FileAdapter; becomes idx in entries.idx
    cfg.paths = { repo_toplevel };
    cfg.extensions = default_index_extensions();
    return resync_disk_index(store, embedder, idx, cfg, /*repo_root=*/repo_toplevel, deadline,
                             progress, root_sha);
}

} // namespace gogol
