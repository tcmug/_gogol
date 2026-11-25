// gogol MCP server — stdio JSON-RPC 2.0 implementation.
//
// Protocol glue only. The Tool Registry (mcp/tool_registry.h) is the single
// source of truth for the tool catalog + param schema. No search/graph logic is
// reimplemented here.
//
// All exposed tools operate over the unified memory store (~/.gogol/cache.db +
// notes.db); each handler reads/writes it directly (no daemon). The v1 index-backed
// tools (query/read/list/affected/metrics/write/rm/move) were retired and their
// handlers removed — the v1 stores are migrated into the v2 memory.
#include "mcp/mcp_server.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "config/config.h"
#include "embedding/embed_provider.h"
#include "core/refresh.h"      // gogol::refresh_repo, kMcpInlineRefreshBudgetMs
#include "core/result_formatters.h"  // gogol::format_recall_results
#include "cli/git_identity.h"        // gogol::resolve_git_identity
#include "mcp/json.h"
#include "mcp/daemon_lock.h"   // DaemonLock (singleton flock)
#include "mcp/unix_server.h"   // run_unix_socket_server
#include "mcp/stdio_shim.h"    // run_stdio_shim
#include "mcp/tool_registry.h"
#include "mcp/message_bus.h"
#include "store/store.h"

namespace gogol {
namespace mcp {

// --- spillover thread tracking ---
// Background refresh threads (MCP inline-refresh budget spillover) are TRACKED,
// not detached, so we can join them before the process exits. A detached thread
// still holding a llama/ggml Metal context at exit() trips the Metal backend's
// static-teardown assert (ggml_metal_device_free expects 0 outstanding sets);
// joining first guarantees no thread owns a Metal context during shutdown.
namespace {
std::mutex g_spillover_mu;
std::vector<std::thread> g_spillover_threads;

void track_spillover(std::thread t) {
    std::lock_guard<std::mutex> lk(g_spillover_mu);
    g_spillover_threads.push_back(std::move(t));
}

void join_spillover_threads() {
    std::vector<std::thread> pending;
    {
        std::lock_guard<std::mutex> lk(g_spillover_mu);
        pending.swap(g_spillover_threads);
    }
    for (auto &t : pending)
        if (t.joinable()) t.join();
}
}  // namespace

namespace {

// The MCP transport is JSON-RPC over stdout: ONLY framed JSON may hit fd 1, or the
// client closes the connection ("non-JSON-RPC output to stdout"). But loading the
// embedder in-process runs llama.cpp/ggml backend init, which can write device/
// backend lines DIRECTLY to stdout (bypassing llama_log_set). StdoutGuard redirects
// fd 1 -> fd 2 (stderr) for its lifetime, so any such stray bytes go to stderr and
// never corrupt the JSON-RPC stream. Wrap embedder acquisition / embed calls in it.
// INVARIANT §5 (docs/INVARIANTS.md): ggml logs to raw stdout, bypassing llama_log_set.
struct StdoutGuard {
    int saved = -1;
    StdoutGuard() {
        std::fflush(stdout);
        saved = ::dup(1);
        ::dup2(2, 1);              // point stdout at stderr
    }
    ~StdoutGuard() {
        std::fflush(stdout);
        if (saved >= 0) { ::dup2(saved, 1); ::close(saved); }  // restore real stdout
    }
    StdoutGuard(const StdoutGuard&) = delete;
    StdoutGuard& operator=(const StdoutGuard&) = delete;
};

// JSON-RPC 2.0 standard error codes.
constexpr int kParseError = -32700;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

const char* kServerName = "gogol";
const char* kServerVersion = "0.1.0";
const char* kProtocolVersion = "2024-11-05";

// Server instructions (see handle_initialize). Front-loaded: tool selection +
// the core workflow rule sit in the first ~512 chars so they're available when
// the client decides how to use the server. Fully generic (no deployment
// specifics). Mirrors the SKILL decision-flow — keep the two aligned.
const char* kInstructions =
    "gogol is a semantic code-search + knowledge store over one or more indexed "
    "codebases. Choose a tool by intent: to understand a named function use "
    "`explore` (signature + source + callers + callees + imports in one call); "
    "to find code by concept/meaning use `query`; to trace call chains use "
    "`calls`; for the blast radius of a change use `affected`; to read a known "
    "location use `read`; to see what an index holds use `list`.\n\n"
    "Core workflow rule: after `query` returns a hit, `read` or `explore` that "
    "exact location \xE2\x80\x94 do NOT re-query with reworded terms (two queries "
    "max, then switch tools). Prefer `explore` over `query` whenever you already "
    "know the symbol name.\n\n"
    "Details: every entry is `<type> <index> <path>` \xE2\x80\x94 type is `doc` "
    "(a file chunk), `note` (a memory note), or `term` (a glossary term). "
    "Results lead with `type index path:line`, directly reusable as `read`/"
    "`explore` input. Pass `index` inline on each call to scope it; omit it to "
    "search all indexes. `affected` "
    "returning 0 usually means the file is an entry point that nothing imports "
    "(usually correct, not an error). `write` persists reusable findings.";

// Build a JSON-RPC success envelope: {jsonrpc, id, result}.
Json make_response(const Json& id, Json result) {
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = std::move(result);
    return r;
}

// Build a JSON-RPC error envelope: {jsonrpc, id, error:{code,message}}.
Json make_error(const Json& id, int code, const std::string& message) {
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = std::move(err);
    return r;
}

// Wrap a ToolResult as the MCP tools/call result:
//   { content: [ { type:"text", text:<serialized json> } ], isError }
Json wrap_tool_result(const ToolResult& tr) {
    Json block = Json::object();
    block["type"] = "text";
    if (tr.is_error) {
        // Serialize the error as a JSON object so the agent gets structured text.
        Json e = Json::object();
        e["error"] = tr.error;
        block["text"] = e.dump();
    } else {
        block["text"] = tr.content.dump();
    }
    Json content = Json::array();
    content.push_back(std::move(block));

    Json result = Json::object();
    result["content"] = std::move(content);
    result["isError"] = tr.is_error;
    return result;
}

// --- Unified memory store. Reads ~/.gogol/cache.db + notes.db directly (no
// daemon); embeds the query via get_embedder. Cross-index hybrid retrieval:
// semantic top-k + call-graph edge expansion. ---
static std::string cache_db_path_mcp() {
    return cache_db_path();
}
static std::string notes_db_path_mcp() {
    return notes_db_path();
}

// Resolve the embedding model path WITHOUT exiting (unlike cli resolve_model):
// env override, else global config.
static std::string resolve_model_path() {
    if (const char* env = std::getenv("GOGOL_MODEL")) return env;
    auto gc = load_global_config();
    if (!gc.model.empty()) return gc.model;
    // Convention fallback: gogol_home()/model symlink, if present.
    std::error_code ec;
    std::string mp = model_path();
    if (std::filesystem::exists(mp, ec) && !ec) return mp;
    return gc.model;  // empty -> caller reports "model not found"
}

// The MCP server is long-lived and handles many tool calls in one process, so the
// model must load ONCE and stay warm — get_shared_embedder returns a process-lifetime
// singleton (keyed by model+prefixes). Returns nullptr only if no model path is set.
static EmbedProvider* get_embedder_cfg() {
    auto gc = load_global_config();
    std::string mp = gc.model;
    if (const char* env = std::getenv("GOGOL_MODEL")) mp = env;
    if (mp.empty()) return nullptr;
    return &get_shared_embedder(mp, gc.query_prefix, gc.document_prefix);
}

ToolResult handle_store_query(const ToolArgs& a) {
    std::string text = a.get_str("text");
    if (text.empty()) return ToolResult::fail("missing required parameter: text");
    std::string dbp = cache_db_path_mcp();
    EmbedProvider* embedder = nullptr;
    std::vector<float> q;
    {
        StdoutGuard guard;  // llama/ggml init + encode may write to stdout — keep it off the JSON-RPC stream
        embedder = get_embedder_cfg();
        if (embedder) q = embedder->embed_query(text);
    }
    if (!embedder) return ToolResult::fail("could not load embedder");

    // MCP on-demand refresh (inline_refresh model): if `scope` is a git repo
    // path, refresh its changed files INLINE with a ~5s budget so fresh edits show in
    // THIS response; if the budget is hit (unfinished work remains), finish the rest on
    // a DETACHED background thread so the call stays responsive. `indexing` in the
    // response signals a spillover is still running. No repo resolvable => skip refresh.
    // This runs BEFORE the store-existence check so a COLD store (no cache.db yet) is
    // created + populated by the refresh — the first recall on a fresh repo indexes it.
    bool indexing = false;
    {
        std::string scope = a.get_str("scope");
        std::string repo, idx;
        if (!scope.empty()) {
            auto gid = gogol::resolve_git_identity(scope);
            if (gid && !gid->toplevel.empty()) { repo = gid->toplevel; idx = gid->common_dir; }
        }
        if (!repo.empty()) {
            gogol::IngestResult ir;
            {
                StdoutGuard guard;  // embed_document during refresh may hit stdout
                // Inline pass owns its OWN writable store handle (the query store below
                // is read-only); shares the process embedder. Connections run on their
                // own threads, so concurrent llama use is prevented by Embedder's mutex
                // (NOT by a single-threaded request loop — there isn't one).
                store::Store rw(dbp, notes_db_path_mcp());
                auto deadline = std::chrono::steady_clock::now() + gogol::kMcpInlineRefreshBudgetMs;
                ir = gogol::refresh_repo(rw, *embedder, repo, idx, deadline);
            }
            if (ir.ok && ir.hit_budget) {
                indexing = true;
                // Spillover: finish the refresh unbounded on a detached thread. It owns
                // its OWN Store (own SQLite connection; WAL + busy_timeout serialize
                // the one writer) AND its OWN fresh embedder (own llama context — NOT the
                // shared one the request loop uses, so no concurrent-context race). The
                // body is wrapped in try/catch: llama.cpp installs a std::set_terminate
                // -> abort(), so an uncaught exception would kill the whole MCP process.
                std::string model = resolve_model_path();
                track_spillover(std::thread([dbp, notes = notes_db_path_mcp(), repo, idx, model]() {
                    try {
                        auto gc = load_global_config();
                        auto emb = get_embedder(model, gc.query_prefix, gc.document_prefix);
                        if (!emb) return;
                        store::Store rw(dbp, notes);
                        gogol::refresh_repo(rw, *emb, repo, idx);  // no deadline => finish it
                    } catch (...) {
                        // swallow: a failed background refresh must not abort the process;
                        // the next query's inline pass will pick up whatever is unfinished.
                    }
                }));  // tracked (not detached): joined before process exit so no
                      // spillover thread holds a Metal context during static teardown.
            }
        }
    }

    // After a possible refresh, the store must exist. If it still doesn't (no scope /
    // not a repo AND no prior store), there is nothing to search.
    std::error_code ec;
    if (!std::filesystem::exists(dbp, ec) || ec)
        return ToolResult::fail("v2 memory store not found at " + dbp +
                                " (pass scope=<a git repo path> to index it)");

    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    int k = a.has("limit") ? static_cast<int>(a.get_int("limit")) : 10;
    if (k <= 0) k = 10;
    std::string rtype = a.get_str("type");   // optional: filter note|doc|code
    // Recall scope resolution (shared with CLI via gogol::resolve_recall_scope). MCP has
    // no CWD (cwd left empty -> no default-scope fallback; a bare non-git `scope` dir
    // becomes a parent-dir search instead).
    auto known = store.index_locations();
    gogol::RecallScopeInputs in;
    in.ref = a.get_str("ref");          // portable root-SHA identity
    in.scope = a.get_str("scope");      // path / repo name / dir
    in.scope_set = !in.scope.empty();
    // in.cwd stays empty — the MCP server has no meaningful CWD.
    auto rsc = gogol::resolve_recall_scope(in, known);
    if (!rsc.error.empty()) return ToolResult::fail(rsc.error);

    std::vector<std::string> restrict_refs =
        store.recall_restrict_refs(rsc.parent_dir, rsc.scoped);
    std::string source_prefix = rsc.source_prefix;
    if (!restrict_refs.empty()) source_prefix.clear();
    auto hits = store.query_hybrid(q, k, rsc.idx, rtype, rsc.note_scope, text, source_prefix, restrict_refs);  // cosine+FTS A3 blend; idx+type PRE-filtered; notes scope-filtered
    
    // Compute the best cosine across all non-edge hits for global confidence flag
    float global_max_cosine = 0.0f;
    for (const auto& h : hits) {
        if (!h.via_edge && h.max_cosine > global_max_cosine)
            global_max_cosine = h.max_cosine;
    }
    // Use unified formatter for consistent CLI + MCP output
    auto formatted = gogol::format_recall_results(hits, store, global_max_cosine);
    Json out = formatted.to_json();
    
    // Add indexing status: true only when background refresh still running
    if (indexing) out["indexing"] = true;
    
    return ToolResult::ok(std::move(out));

}

// Forward decl: the message bus singleton is defined below, but status reads it.
static MessageBus& message_bus();

// `[mcp] messaging` (agent messaging over MCP). Read from config once per process, like
// the rest of the daemon's config; set_messaging_override pins it (tests).
static int g_messaging_override = -1;  // -1 = read config, 0/1 = pinned
static bool messaging_enabled() {
    if (g_messaging_override >= 0) return g_messaging_override == 1;
    static const bool from_config = load_global_config().messaging;
    return from_config;
}

ToolResult handle_store_stats(const ToolArgs&) {
    Json wrapped = Json::object();
    // Store section: present only if the cache.db exists (bus stats work even cold).
    std::string dbp = cache_db_path_mcp();
    std::error_code ec;
    if (std::filesystem::exists(dbp, ec) && !ec) {
        store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
        auto s = store.stats();
        Json out = Json::object();
        out["entries"] = static_cast<long>(s.entries);
        out["vec_entries"] = static_cast<long>(s.vec_entries);
        out["edges"] = static_cast<long>(s.edges);
        out["indexes"] = static_cast<long>(s.indexes);
        out["notes"] = static_cast<long>(store.count_by_type("note"));
        out["path"] = dbp;
        // Per-index breakdown: each index's portable ref (root_sha), on-disk checkout
        // dir(s), overlay flag, and entry count. root_sha is the token to pass as `scope`/
        // `ref` from a machine with no shared filesystem.
        Json ix = Json::array();
        for (const auto &info : store.index_breakdown()) {
            Json o = Json::object();
            o["idx"] = info.idx;
            if (!info.root_sha.empty()) o["root_sha"] = info.root_sha;
            o["entries"] = static_cast<long>(info.entries);
            Json dirs = Json::array();
            for (const auto &d : info.dirs) {
                Json dj = Json::object();
                dj["dir"] = d.path;
                dj["overlay"] = d.overlay;
                dirs.push_back(std::move(dj));
            }
            o["dirs"] = std::move(dirs);
            ix.push_back(std::move(o));
        }
        out["index_list"] = std::move(ix);
        wrapped["store"] = std::move(out);
    }

    // Bus section: combadge agent message bus observability (always present).
    BusStats bs = message_bus().stats();
    Json bus = Json::object();
    bus["agents"] = bs.agents;
    bus["online"] = bs.online;
    bus["total_sent"] = bs.total_sent;
    bus["queued"] = bs.queued;
    Json agents = Json::array();
    for (const auto& r : bs.rows) {
        Json a = Json::object();
        a["name"] = r.name;
        a["online"] = r.online;
        a["inbox_depth"] = r.inbox_depth;
        a["sent"] = r.sent;
        a["received"] = r.received;
        a["last_seen"] = r.last_seen;
        agents.push_back(std::move(a));
    }
    bus["agents_list"] = std::move(agents);
    wrapped["bus"] = std::move(bus);

    return ToolResult::ok(std::move(wrapped));
}

// --- combadge agent message bus (shared across all serve threads) ---
// Process-global: gogol serve is thread-per-connection, so a blocked `receive` parks
// its own thread on the bus's per-name condvar. The daemon (cmd_serve) points the bus
// at messages.db before first use so queued messages survive a restart (ADR-003).
// Any other process (the in-process stdio fallback) gets a private in-memory bus: it
// cannot share wakeups with the daemon, so sharing its file would only mislead.
static std::string g_bus_db_path = ":memory:";

static MessageBus& message_bus() {
    static MessageBus& bus = []() -> MessageBus& {
        try {
            return *new MessageBus(g_bus_db_path);
        } catch (const std::exception& e) {
            fprintf(stderr, "gogol: cannot open message store %s (%s); bus is in-memory.\n",
                    g_bus_db_path.c_str(), e.what());
            return *new MessageBus();
        }
    }();
    return bus;
}

static Json bus_msg_json(const BusMsg& m) {
    Json j = Json::object();
    j["seq"] = m.seq;
    j["from"] = m.from;
    j["body"] = m.body;
    j["ts"] = m.ts;
    return j;
}

ToolResult handle_msg_send(const ToolArgs& a) {
    std::string to = a.get_str("to");
    if (to.empty()) return ToolResult::fail("missing required parameter: to");
    // `me` (the handle from listen) wins over `from`: a client that injects `me` decides
    // who the sender is, whatever the model put in `from`.
    std::string from = a.get_str("me");
    if (from.empty()) from = a.get_str("from", "anon");
    std::string body = a.get_str("body");

    // Selling point: semantic-context-on-send. Attach recall hits for the body so the
    // recipient arrives pre-loaded and skips its own search turns. Reuses the SAME
    // recall path (handle_store_query) scoped by ref (portable, no shared fs needed).
    Json context;
    bool have_context = false;
    if (a.get_bool("with_context")) {
        long climit = a.has("context_limit") ? a.get_int("context_limit") : 5;
        if (climit <= 0) climit = 5;
        Json qargs = Json::object();
        qargs["text"] = body;
        qargs["limit"] = climit;
        if (a.has("ref")) qargs["ref"] = a.get_str("ref");
        ToolArgs qa{std::move(qargs)};
        ToolResult qr = handle_store_query(qa);
        if (!qr.is_error) { context = std::move(qr.content); have_context = true; }
        // A failed recall (no store/embedder) must NOT block delivery — send anyway,
        // just without context. The message is the point; context is a bonus.
    }

    SendOutcome s = message_bus().send(to, from, body);
    Json out = Json::object();
    out["seq"] = s.seq;
    out["delivered_live"] = s.delivered_live;
    out["queued_depth"] = s.queued_depth;
    out["your_inbox"] = s.your_inbox;
    if (have_context) out["context"] = std::move(context);
    return ToolResult::ok(std::move(out));
}

// listen: claim an agent name for messaging over MCP. Stateless (SEP-2567 handle pattern):
// nothing is stored per connection; the returned `me` is the handle the caller passes back.
// Marks the name online so senders see delivered_live, and reports the current backlog.
ToolResult handle_msg_listen(const ToolArgs& a) {
    std::string name = a.get_str("name");
    if (name.empty()) return ToolResult::fail("missing required parameter: name");
    message_bus().set_online(name, true);
    Json out = Json::object();
    out["me"] = name;
    out["unread"] = static_cast<long>(message_bus().peek(name).messages.size());
    out["hint"] = "Pass me=\"" + name + "\" on later gogol calls to send as this name "
                  "and to be told when messages are waiting.";
    return ToolResult::ok(std::move(out));
}

ToolResult handle_msg_receive(const ToolArgs& a) {
    std::string me = a.get_str("me");
    if (me.empty()) return ToolResult::fail("missing required parameter: me");
    long timeout_ms = a.has("timeout_ms") ? a.get_int("timeout_ms") : 0;
    bool consume = a.has("consume") ? a.get_bool("consume") : true;
    std::string session = a.get_str("session");
    ReceiveOutcome r = message_bus().receive(me, timeout_ms, consume, 300000, session);
    Json msgs = Json::array();
    for (const BusMsg& m : r.messages) msgs.push_back(bus_msg_json(m));
    Json out = Json::object();
    out["messages"] = std::move(msgs);
    out["timed_out"] = r.timed_out;
    return ToolResult::ok(std::move(out));
}

// ack: confirm the listed seqs (claimed via a session receive) were handled, so they
// are never redelivered. Unknown, foreign or already-acked seqs are ignored.
ToolResult handle_msg_ack(const ToolArgs& a) {
    std::string me = a.get_str("me");
    if (me.empty()) return ToolResult::fail("missing required parameter: me");
    std::vector<int64_t> seqs;
    auto it = a.obj.find("seq");
    if (it != a.obj.end()) {
        if (it->is_number_integer()) seqs.push_back(it->get<int64_t>());
        else if (it->is_array())
            for (const auto& v : *it)
                if (v.is_number_integer()) seqs.push_back(v.get<int64_t>());
    }
    if (seqs.empty()) return ToolResult::fail("missing required parameter: seq");
    Json out = Json::object();
    out["acked"] = message_bus().ack(me, seqs);
    return ToolResult::ok(std::move(out));
}

ToolResult handle_msg_peek(const ToolArgs& a) {
    std::string me = a.get_str("me");
    if (me.empty()) return ToolResult::fail("missing required parameter: me");
    ReceiveOutcome r = message_bus().peek(me);
    Json msgs = Json::array();
    for (const BusMsg& m : r.messages) msgs.push_back(bus_msg_json(m));
    Json out = Json::object();
    out["messages"] = std::move(msgs);
    out["count"] = static_cast<long>(r.messages.size());
    return ToolResult::ok(std::move(out));
}

// presence: set a name online/offline (when `online` is present), OR query it (when not).
// Advisory — lets send() report delivered_live before a receive is parked.
ToolResult handle_msg_presence(const ToolArgs& a) {
    std::string name = a.get_str("name");
    if (name.empty()) return ToolResult::fail("missing required parameter: name");
    if (a.has("online")) message_bus().set_online(name, a.get_bool("online"));
    Json out = Json::object();
    out["name"] = name;
    out["online"] = message_bus().is_online(name);
    return ToolResult::ok(std::move(out));
}

ToolResult handle_store_read(const ToolArgs& a) {
    std::string id = a.get_str("id");
    if (id.empty()) return ToolResult::fail("missing required parameter: id");
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    auto e = store.get_latest(id);
    if (!e) return ToolResult::fail("no entry with id " + id);
    Json out = Json::object();
    out["id"] = e->id; out["type"] = e->type; out["source"] = e->source;
    out["content"] = gogol::display_content(e->content);  // strip embed-enrichment scaffolding for display
    if (!e->supersedes.empty()) out["supersedes"] = e->supersedes;
    if (!e->superseded_by.empty()) out["superseded_by"] = e->superseded_by;
    if (!e->resolved_from.empty()) out["resolved_from"] = e->resolved_from;
    Json ix = Json::array(); for (auto& n : e->indexes) ix.push_back(n);
    out["indexes"] = std::move(ix);
    return ToolResult::ok(std::move(out));
}

// Resolve a SINGLE index for the symbol tools (delve/links/grep) from the shared
// scope resolver reading `scope` (name/path) + `ref` (root SHA). MCP has no CWD, so
// scope-by-path can't resolve server-side — a repo NAME or a --ref SHA is the way in.
// Replaces the retired `index` param; an unresolved scope => none() = all indexes.
// On an INVALID path scope (nonexistent, not a repo/dir) *err is set when non-null so
// the caller can fail — same "bad path is an error" rule as the CLI.
static gogol::ResolvedIdx mcp_resolve_idx(const ToolArgs& a, store::Store& store,
                                          std::string* err = nullptr) {
    gogol::RecallScopeInputs in;
    in.ref = a.get_str("ref");
    in.scope = a.get_str("scope");
    in.scope_set = !in.scope.empty();
    // in.cwd stays empty — the MCP server has no meaningful CWD.
    auto rsc = gogol::resolve_recall_scope(in, store.index_locations());
    if (err) *err = rsc.error;
    return rsc.idx;
}

ToolResult handle_store_links(const ToolArgs& a) {
    std::string node = a.get_str("node");
    if (node.empty()) node = a.get_str("symbol"); // back-compat alias
    if (node.empty()) return ToolResult::fail("missing required parameter: node");
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    std::string kind = a.get_str("kind");        // "" = any (call|import|docref)
    std::string dir = a.get_str("direction");
    if (dir.empty()) dir = "both";
    int depth = a.has("depth") ? static_cast<int>(a.get_int("depth")) : 1;
    if (depth < 1) depth = 1;
    std::string scope_err; gogol::ResolvedIdx index = mcp_resolve_idx(a, store, &scope_err);
    if (!scope_err.empty()) return ToolResult::fail(scope_err);
    std::string tail = store::symbol_tail(node);
    auto rows = store.links(tail, kind, dir, depth, index);
    if (tail != node) {
        auto raw = store.links(node, kind, dir, depth, index);
        rows.insert(rows.end(), raw.begin(), raw.end());
    }
    Json out_links = Json::array();
    for (auto& e : rows) {
        Json o = Json::object();
        o["kind"] = e.kind; o["from"] = e.src; o["to"] = e.dst;
        o["index"] = e.idx;
        out_links.push_back(std::move(o));
    }
    Json out = Json::object();
    out["node"] = node; out["direction"] = dir;
    if (!kind.empty()) out["kind"] = kind;
    out["links"] = std::move(out_links);
    out["count"] = rows.size();
    return ToolResult::ok(std::move(out));
}

ToolResult handle_store_metrics(const ToolArgs& a) {
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    int limit = a.has("limit") ? static_cast<int>(a.get_int("limit")) : 20;
    if (limit <= 0) limit = 20;
    auto rows = store.top_complexity(limit, a.get_str("filter"));
    Json arr = Json::array();
    for (auto& m : rows) {
        Json o = Json::object();
        o["file"] = m.file; o["name"] = m.name; o["line"] = m.line;
        o["complexity"] = m.complexity; o["lines"] = m.lines;
        o["params"] = m.params; o["returns"] = m.returns; o["max_depth"] = m.max_depth;
        arr.push_back(std::move(o));
    }
    Json out = Json::object();
    out["functions"] = std::move(arr); out["count"] = rows.size();
    return ToolResult::ok(std::move(out));
}

// Deep-dive a symbol: definition/source + callers + callees + imports, composed
// entirely from EXISTING store machinery (ids_for_symbol -> get for the definition;
// links for the call/import graph). No new graph code. The "definition" is the
// entry the symbol maps to in symbols (its own defining chunk); callers/callees
// come from the call graph, imports from the import graph of the defining file.
ToolResult handle_store_delve(const ToolArgs& a) {
    std::string symbol = a.get_str("symbol");
    if (symbol.empty()) symbol = a.get_str("name");   // accept either param name
    if (symbol.empty()) return ToolResult::fail("missing required parameter: symbol");
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    std::string scope_err; gogol::ResolvedIdx index = mcp_resolve_idx(a, store, &scope_err);
    if (!scope_err.empty()) return ToolResult::fail(scope_err);
    std::string tail = store::symbol_tail(symbol);

    // Definition/source: the entry(ies) the symbol maps to (its own defining chunk).
    Json defs = Json::array();
    std::string def_source;
    auto ids = store.ids_for_symbol(tail);
    if (tail != symbol) {
        auto raw = store.ids_for_symbol(symbol);
        ids.insert(ids.end(), raw.begin(), raw.end());
    }
    for (const auto& id : ids) {
        auto e = store.get(id);
        if (!e) continue;
        Json d = Json::object();
        d["id"] = e->id; d["type"] = e->type; d["source"] = e->source;
        if (e->line > 0) d["line"] = static_cast<long>(e->line);
        if (e->end_line > 0) d["end_line"] = static_cast<long>(e->end_line);
        d["content"] = e->content;
        if (def_source.empty()) def_source = e->source;
        defs.push_back(std::move(d));
    }

    // Callers/callees from the call graph; imports of the defining file from the
    // import graph. All via the existing links() query — no new graph code.
    auto to_json = [](const std::vector<store::Store::Link>& rows) {
        Json arr = Json::array();
        for (const auto& e : rows) {
            Json o = Json::object();
            o["kind"] = e.kind; o["from"] = e.src; o["to"] = e.dst; o["index"] = e.idx;
            arr.push_back(std::move(o));
        }
        return arr;
    };
    auto callers = store.links(tail, "call", "in", 1, index);
    auto callees = store.links(tail, "call", "out", 1, index);
    auto imports = def_source.empty() ? std::vector<store::Store::Link>{}
                                      : store.links(def_source, "import", "both", 1, index);

    Json out = Json::object();
    out["symbol"] = symbol;
    out["definitions"] = std::move(defs);
    out["callers"] = to_json(callers);
    out["callees"] = to_json(callees);
    out["imports"] = to_json(imports);
    return ToolResult::ok(std::move(out));
}

ToolResult handle_store_forget(const ToolArgs& a) {
    std::string id = a.get_str("id");
    if (id.empty()) return ToolResult::fail("missing required parameter: id");
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp());
    int rc = store.forget_note(id);
    if (rc == 0) return ToolResult::fail("no entry with id " + id);
    if (rc == -1) return ToolResult::fail(
        "id " + id + " is a disk-sourced entry (code/doc), not a note — "
        "forget only removes LLM-authored notes; edit/delete the source file instead");
    Json out = Json::object(); out["forgotten"] = id; out["ok"] = true;
    return ToolResult::ok(std::move(out));
}

ToolResult handle_store_related(const ToolArgs& a) {
    std::string id = a.get_str("id");
    if (id.empty()) return ToolResult::fail("missing required parameter: id");
    int limit = a.has("limit") ? static_cast<int>(a.get_int("limit")) : 10;
    if (limit < 1) limit = 10;
    std::string type_filter = a.get_str("type"); // "code", "doc", or empty for both
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    auto hits = store.related(id, limit, type_filter);
    if (hits.empty()) return ToolResult::fail("no entry with id " + id + " or no embedding");
    Json arr = Json::array();
    for (const auto& hit : hits) {
        auto e = store.get(hit.id);
        if (!e) continue;
        Json obj = Json::object();
        obj["id"] = e->id;
        obj["score"] = hit.score;
        obj["type"] = e->type;
        obj["source"] = e->source;
        if (!e->symbol.empty()) obj["symbol"] = e->symbol;
        if (e->line > 0) obj["line"] = e->line;
        arr.push_back(std::move(obj));
    }
    return ToolResult::ok(std::move(arr));
}

ToolResult handle_store_grep(const ToolArgs& a) {
    std::string pattern = a.get_str("pattern");
    if (pattern.empty()) return ToolResult::fail("missing required parameter: pattern");
    int limit = a.has("limit") ? static_cast<int>(a.get_int("limit")) : 50;
    if (limit < 1) limit = 50;
    std::string dbp = cache_db_path_mcp();
    std::error_code ec; if (!std::filesystem::exists(dbp, ec) || ec) return ToolResult::fail("v2 memory store not found");
    store::Store store(dbp, notes_db_path_mcp(), store::Store::ReadOnly{});
    std::string scope_err; gogol::ResolvedIdx index = mcp_resolve_idx(a, store, &scope_err);
    if (!scope_err.empty()) return ToolResult::fail(scope_err);
    auto hits = store.literal_search(pattern, limit, index);
    Json arr = Json::array();
    for (const auto& h : hits) {
        Json obj = Json::object();
        obj["source"] = h.source;
        obj["line"] = h.line;
        if (!h.symbol.empty()) obj["symbol"] = h.symbol;
        obj["id"] = h.id;
        arr.push_back(std::move(obj));
    }
    Json out = Json::object();
    out["results"] = std::move(arr);
    out["count"] = static_cast<long>(hits.size());
    return ToolResult::ok(std::move(out));
}

ToolResult handle_store_add(const ToolArgs& a) {
    std::string content = a.get_str("content");
    if (content.empty()) return ToolResult::fail("missing required parameter: content");
    std::string consolidate_id = a.get_str("consolidate");
    EmbedProvider* embedder = nullptr;
    std::vector<float> v;
    {
        StdoutGuard guard;  // keep llama/ggml stdout off the JSON-RPC stream
        embedder = get_embedder_cfg();
        if (embedder) v = embedder->embed_document(content);
    }
    if (!embedder) return ToolResult::fail("could not load embedder");
    store::Store store(cache_db_path_mcp(), notes_db_path_mcp());
    // LLM-authored note -> notes.db (content of record). Optional path scope drives
    // path-prefix surfacing at query time; empty => a global note. The MCP server has
    // no reliable CWD, so scope must be passed explicitly (unlike the CLI, which
    // defaults to the CWD's git toplevel).
    std::string scope = a.get_str("scope");
    Json out = Json::object();
    if (consolidate_id.empty()) {
        std::string id = store.put_note(content, v, scope);
        store.commit();
        out["id"] = id;
    } else {
        // consolidate: forget(old) + put(new) in one Tx (see Store::consolidate_note).
        auto r = store.consolidate_note(consolidate_id, content, v, scope);
        store.commit();
        out["id"] = r.id;
        out["forgotten"] = r.forgotten;
        out["refused"] = r.refused;
        out["old_id"] = consolidate_id;
    }
    out["type"] = "note"; out["stored"] = true;
    out["scope"] = scope;
    out["notes"] = static_cast<long>(store.count_by_type("note"));
    return ToolResult::ok(std::move(out));
}

// The SINGLE dispatch site: one name→handler table, built once. Adding a tool
// means adding its ToolDef to the registry (catalog/schema) and one entry here
// (behavior). tools/call resolves the handler through this — no scattered
// per-tool branching elsewhere.
ToolResult tool_dispatch(const std::string& name, const ToolArgs& args) {
    static const std::unordered_map<std::string,
        std::function<ToolResult(const ToolArgs&)>> handlers = {
        // v1 index tools retired (stores migrated to v2) — their handlers have
        // been removed from this file. Only the store_* handlers below remain.
        {"recall",         handle_store_query},   // search memory (hybrid)
        {"get",            handle_store_read},    // fetch one entry by id
        {"listen",         handle_msg_listen},    // messaging over MCP: claim a name, get `me`
        {"send",           handle_msg_send},      // combadge: send a message (opt. semantic context)
        {"receive",        handle_msg_receive},   // combadge: block for a message (long-poll)
        {"ack",            handle_msg_ack},       // combadge: confirm session-claimed messages
        {"peek",           handle_msg_peek},      // combadge: list queued messages, no consume
        {"presence",       handle_msg_presence},  // combadge: set/query a name's listening state
        {"delve",          handle_store_delve},   // deep-dive a symbol: def + callers/callees/imports
        {"links",          handle_store_links},   // unified graph: call|import|docref, in/out/both
        {"metrics",        handle_store_metrics}, // complexity ranking (quality signal)
        {"remember",       handle_store_add},     // author a note-document (consolidate= replaces one atomically)
        {"forget",         handle_store_forget},  // remove an LLM-authored note by id
        {"related",        handle_store_related}, // find entries related by embedding similarity
        {"literal",        handle_store_grep},    // literal phrase search (exact match)
        {"status",         handle_store_stats},
    };
    auto it = handlers.find(name);
    if (it == handlers.end()) return ToolResult::fail("unknown tool: " + name);
    return it->second(args);
}

// Find a tool def by name in the registry (nullptr if absent).
const ToolDef* find_tool(const std::string& name) {
    for (const ToolDef& def : tool_registry())
        if (def.name == name) return &def;
    return nullptr;
}

// Validate that all required params are present (and non-empty for strings).
// Returns "" if valid, else a human-readable error naming the missing param.
std::string validate_required(const ToolDef& def, const ToolArgs& args) {
    for (const ToolParam& p : def.params) {
        if (!p.required) continue;
        if (!args.has(p.name))
            return "missing required parameter: " + p.name;
        // A required string present but empty is also invalid.
        if (p.type == "string" && args.get_str(p.name).empty())
            return "empty required parameter: " + p.name;
    }
    return "";
}

// --- Method handlers ---

Json handle_initialize(const Json& id) {
    Json server_info = Json::object();
    server_info["name"] = kServerName;
    server_info["version"] = kServerVersion;

    Json tools_cap = Json::object();  // {} — tools capability present
    Json capabilities = Json::object();
    capabilities["tools"] = std::move(tools_cap);

    Json result = Json::object();
    result["protocolVersion"] = kProtocolVersion;
    result["serverInfo"] = std::move(server_info);
    result["capabilities"] = std::move(capabilities);
    // Server instructions (MCP spec: optional guidance injected into the model's
    // context by clients that support it). Front-loads the cross-tool workflow —
    // which tool for which intent + the "don't re-query" rule — in the first
    // ~512 chars, per ecosystem best practice. Generic: no deployment specifics.
    result["instructions"] = kInstructions;
    return make_response(id, std::move(result));
}

Json handle_tools_list(const Json& id) {
    Json result = Json::object();
    result["tools"] = tools_list_schema(messaging_enabled());
    return make_response(id, std::move(result));
}

// Messaging over MCP: when the caller passes `me` (the handle from listen), every tool
// result also says how many messages wait for it, as a second text block the model sees.
// Shown once per change (MessageBus::unread_hint). Not for the bus tools themselves,
// which already report the inbox.
void append_unread_hint(const std::string& tool, const ToolArgs& args, Json& result) {
    if (!messaging_enabled()) return;
    if (tool == "listen" || tool == "receive" || tool == "peek" || tool == "ack" ||
        tool == "presence")
        return;
    const std::string me = args.get_str("me");
    if (me.empty()) return;
    auto hint = message_bus().unread_hint(me);
    if (!hint) return;
    std::string from;
    for (const auto& f : hint->from) from += (from.empty() ? "" : ", ") + f;
    Json block = Json::object();
    block["type"] = "text";
    block["text"] = "\xF0\x9F\x93\x9F " + std::to_string(hint->count) + " unread message" +
                    (hint->count == 1 ? "" : "s") + " for " + me + " (from: " + from + ").";
    result["content"].push_back(std::move(block));
}

Json handle_tools_call(const Json& id, const Json& params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
        return make_error(id, kInvalidParams, "tools/call requires a string 'name'");

    std::string name = params["name"].get<std::string>();
    const ToolDef* def = find_tool(name);
    if (!def)
        return make_error(id, kMethodNotFound, "unknown tool: " + name);

    ToolArgs args;
    if (params.contains("arguments") && params["arguments"].is_object())
        args.obj = params["arguments"];
    else
        args.obj = Json::object();

    std::string verr = validate_required(*def, args);
    if (!verr.empty())
        return make_error(id, kInvalidParams, verr);

    // Single dispatch point: resolve the handler by name via tool_dispatch().
    // The registry validated existence + params + the read/write gate above;
    // execution is bound in one table (tool_dispatch) — adding a tool is its
    // ToolDef (catalog/schema) + one dispatch entry.
    ToolResult tr = tool_dispatch(name, args);
    Json result = wrap_tool_result(tr);
    append_unread_hint(name, args, result);
    return make_response(id, std::move(result));
}

}  // namespace

void set_bus_db_path(const std::string& path) { g_bus_db_path = path; }
void set_messaging_override(int on) { g_messaging_override = on; }
int requeue_bus_claims() { return message_bus().requeue_all_claims(); }

void start_bus_sweeper() {
    std::thread([]() {
        for (;;) {
            long now = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            try {
                auto r = message_bus().sweep(now);
                if (r.requeued || r.pruned)
                    fprintf(stderr, "gogol daemon: bus sweep requeued %d, pruned %d\n",
                            r.requeued, r.pruned);
            } catch (const std::exception& e) {
                fprintf(stderr, "gogol daemon: bus sweep failed: %s\n", e.what());
            }
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }).detach();
}

// Process a single already-parsed JSON-RPC request object. `has_id` reports
// whether the message carries an id (notifications have none → no response).
// Returns true if a response should be written (via `out_response`).
bool process_request(const Json& msg, Json& out_response) {
    // id is echoed back; null for notifications / when absent.
    Json id = (msg.is_object() && msg.contains("id")) ? msg["id"] : Json(nullptr);

    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        out_response = make_error(id, kInvalidParams, "missing 'method'");
        return true;
    }

    std::string method = msg["method"].get<std::string>();
    Json params = msg.contains("params") ? msg["params"] : Json::object();

    // Notifications (no id) get no response per JSON-RPC 2.0.
    bool is_notification = !(msg.contains("id"));

    if (method == "initialize") {
        out_response = handle_initialize(id);
        return true;
    }
    if (method == "notifications/initialized" || method == "initialized") {
        // Client ack after initialize — a notification, no response.
        return false;
    }
    if (method == "tools/list") {
        out_response = handle_tools_list(id);
        return true;
    }
    if (method == "tools/call") {
        out_response = handle_tools_call(id, params);
        return true;
    }

    if (is_notification) return false;  // unknown notification: ignore silently
    out_response = make_error(id, kMethodNotFound, "unknown method: " + method);
    return true;
}

// --- idle-timeout (daemon self-exit when no activity) ---
// A process-global monotonic "last request" stamp bumped by handle_request_json (the one
// choke-point every transport routes through). The daemon arms an idle watcher that
// _Exit()s the process once (now - last_activity) exceeds the timeout. No refcount: an
// idle timer can't be wedged by a crashed client (unlike a connection count). The shim
// transparently respawns the daemon on the next request, so idle-exit is invisible.
static std::atomic<long> g_last_activity{0};
static long mono_secs() {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void touch_activity() { g_last_activity.store(mono_secs(), std::memory_order_relaxed); }

void start_idle_watcher(int timeout_secs) {
    if (timeout_secs <= 0) return;  // 0 = never idle-exit
    touch_activity();               // start the clock now
    std::thread([timeout_secs]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            long idle = mono_secs() - g_last_activity.load(std::memory_order_relaxed);
            if (idle >= timeout_secs) {
                fprintf(stderr, "gogol daemon: idle %lds >= %ds, exiting.\n", idle, timeout_secs);
                std::_Exit(0);  // skip fragile Metal static teardown (INVARIANT §6)
            }
        }
    }).detach();
}

// Build identity for the daemon<->shim version handshake (external linkage — the shim
// calls THIS same function, so both sides get the identical string from one TU; defining
// it per-TU would risk __DATE__/__TIME__ skew between the daemon and shim builds).
// Compile timestamp => changes on every rebuild, so a rebuilt shim detects a stale daemon.
std::string daemon_build_id() {
    return std::string(kServerVersion) + "+" + __DATE__ " " __TIME__;
}

// Transport-shared: parse + dispatch + error-wrap one raw request string. The ONE place
// the parse/process/try-catch logic lives; stdio, HTTP, and the unix socket all call it.
std::optional<std::string> handle_request_json(const std::string& raw) {
    touch_activity();  // any request resets the idle clock
    Json msg;
    std::string parse_err;
    if (!json_parse(raw, msg, &parse_err)) {
        Json resp = make_error(Json(nullptr), kParseError, "parse error: " + parse_err);
        return resp.dump();
    }
    // Daemon<->shim version handshake: the shim sends {method:"__daemon_version",
    // params:{client_build:"..."}} as its FIRST request. If the client's build differs
    // from ours, WE are the stale daemon (the shim was rebuilt) — report mismatch and
    // self-exit shortly after responding so the shim respawns a fresh daemon. A matching
    // build just confirms. This is NOT a normal tool; handled before dispatch.
    if (msg.is_object() && msg.value("method", std::string()) == "__daemon_version") {
        std::string mine = daemon_build_id();
        std::string theirs = (msg.contains("params") && msg["params"].is_object())
            ? msg["params"].value("client_build", std::string()) : std::string();
        bool match = theirs.empty() || theirs == mine;
        Json resp = Json::object();
        resp["jsonrpc"] = "2.0";
        resp["id"] = (msg.contains("id") ? msg["id"] : Json(nullptr));
        Json r = Json::object();
        r["build"] = mine;
        r["match"] = match;
        resp["result"] = std::move(r);
        if (!match) {
            // Stale daemon: exit shortly so the response flushes first, then the shim
            // respawns the current binary. Detached timer thread.
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                fprintf(stderr, "gogol daemon: build mismatch with client, exiting for respawn.\n");
                std::_Exit(0);
            }).detach();
        }
        return resp.dump();
    }
    Json response;
    bool have_response = false;
    try {
        have_response = process_request(msg, response);
    } catch (const std::exception& e) {
        Json id = (msg.is_object() && msg.contains("id")) ? msg["id"] : Json(nullptr);
        response = make_error(id, kInternalError, std::string("internal error: ") + e.what());
        have_response = true;
    }
    if (!have_response) return std::nullopt;  // notification: no response
    return response.dump();
}

int run_stdio_server(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        // Skip blank lines between messages.
        if (line.empty()) continue;
        // Parse + dispatch + error-wrap via the shared handler (one logic site).
        if (auto resp = handle_request_json(line)) {
            out << *resp << "\n";
            out.flush();
        }
    }
    // stdin EOF: the client is done. Join any outstanding spillover refresh
    // threads so none holds a llama/ggml Metal context during process teardown
    // (that trips the Metal backend's static-destructor assert). Progress is
    // durable per-file, so even if a join takes a moment it only finishes work.
    join_spillover_threads();
    return 0;
}

}  // namespace mcp
}  // namespace gogol

int cmd_mcp() {
    // The MCP server is off unless explicitly enabled in ~/.gogol/config.
    // Refuse to start with a clear message BEFORE entering the stdio loop.
    // `gogol mcp` is a local stdio tool — all tools are exposed once enabled
    // (no read/write gate).
    GlobalConfig gc = load_global_config();
    if (!gc.mcp_enabled) {
        std::cerr << "MCP server is disabled. Enable it with [mcp] enabled = true "
                     "in ~/.gogol/config\n";
        return 1;
    }

    // Daemon mode: if [mcp] socket is enabled, `gogol mcp` is a thin SHIM — it connects
    // to (or spawns) the warm daemon over the unix socket instead of loading a model
    // in-process. This is what makes per-chat stdio cheap AND keeps one SQLite writer.
    // If the shim can't reach/start the daemon, fall back to in-process stdio (below) so
    // the agent is never left without gogol.
    if (gc.socket_enabled) {
        std::string socket_path = gc.socket_path.empty()
            ? (gogol_home() + "/daemon.sock") : gc.socket_path;
        int rc = gogol::mcp::run_stdio_shim(socket_path);
        if (rc == 0) { std::cout.flush(); std::_Exit(0); }
        std::cerr << "gogol: daemon shim failed — falling back to in-process stdio.\n";
        // fall through to in-process below.
    }

    int rc = gogol::mcp::run_stdio_server(std::cin, std::cout);
    // INVARIANT §6 (docs/INVARIANTS.md): short-lived Metal tools must _Exit.
    // Bypass C++ static-destructor teardown: the vendored llama/ggml Metal backend
    // asserts in its static dtor (ggml_metal_device_free) during __cxa_finalize,
    // aborting an otherwise-clean exit. All output is already flushed (the loop
    // flushes per-response + at EOF) and spillover threads are joined, so there is
    // nothing left to clean up in-process — the OS reclaims everything. _Exit skips
    // the fragile static teardown. (If gogol later gains non-Metal state that needs
    // a graceful dtor, revisit — today the only such state is the embedder, whose
    // teardown is exactly the thing that aborts.)
    std::cout.flush();
    std::_Exit(rc);
}

int cmd_serve(const std::string& host, int port) {
    // THE DAEMON: a flock-guarded singleton holding the warm embedder + SQLite (sole
    // writer), serving whichever transports the config enables — a local UNIX SOCKET
    // (thin stdio shims connect; no TCP, no LAN exposure) AND/OR HTTP (remote + dashboard).
    // Both are blocking accept loops, so each runs on its own thread; the main thread waits.
    GlobalConfig gc = load_global_config();

    // Singleton: only one daemon per gogol home may own the store (SQLite single-writer).
    // flock auto-releases on process death — no stale lock, no spawn race. If we lose it,
    // another daemon is already serving; exit quietly (the shim will connect to it).
    std::string lock_path = gogol_home() + "/daemon.lock";
    gogol::mcp::DaemonLock lock(lock_path);
    if (!lock.has_lock()) {
        fprintf(stderr, "gogol serve: another daemon already holds %s — not starting a second.\n",
                lock_path.c_str());
        return 0;  // not an error: the existing daemon is the singleton.
    }

    // Resolve the socket path (config override, else gogol_home/daemon.sock).
    std::string socket_path = gc.socket_path.empty()
        ? (gogol_home() + "/daemon.sock") : gc.socket_path;

    // Which transports? CLI --host/--port drive HTTP (back-compat: `gogol serve` with no
    // [mcp] socket still serves HTTP as before). socket_enabled adds the unix socket.
    bool want_http = port > 0;
    bool want_socket = gc.socket_enabled;
    if (!want_http && !want_socket) {
        // No transport configured at all: preserve the old default (HTTP on the given port).
        want_http = true;
    }
    if (want_http && host != "127.0.0.1" && host != "localhost")
        fprintf(stderr, "WARNING: binding %s exposes recall/notes with NO auth.\n", host.c_str());

    // Persistent message bus: open messages.db now (under the singleton lock, so this
    // process is its only writer) and requeue claims left by sessions of the previous
    // daemon, which cannot ack anymore.
    gogol::mcp::set_bus_db_path(messages_db_path());
    int requeued = gogol::mcp::requeue_bus_claims();
    if (requeued > 0) fprintf(stderr, "gogol daemon: requeued %d unacked message(s)\n", requeued);
    gogol::mcp::start_bus_sweeper();  // now, then hourly

    // Arm idle-timeout (config idle_timeout_secs; 0 = never). The daemon self-exits when
    // no request has arrived for that long; the shim respawns it on the next request.
    gogol::mcp::start_idle_watcher(gc.idle_timeout_secs);

    std::vector<std::thread> transports;
    if (want_socket) {
        fprintf(stderr, "gogol daemon: unix socket %s\n", socket_path.c_str());
        transports.emplace_back([socket_path]() { gogol::mcp::run_unix_socket_server(socket_path); });
    }
    if (want_http) {
        transports.emplace_back([host, port]() { gogol::mcp::run_http_server(host, port); });
    }
    // Wait on the transports forever (they're infinite accept loops). Join blocks the
    // main thread; the process exits via _Exit from a transport error or a signal.
    for (auto& t : transports) t.join();

    std::cout.flush();
    std::_Exit(0);
}
