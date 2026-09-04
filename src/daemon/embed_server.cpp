// embed_server.cpp — Daemon: holds model + indexes in memory, handles all commands
#include "config/debug.h"
#include "daemon/embed_server.h"
#include "embedding/embed_provider.h"
#include "embedding/embedder.h"
#include "embedding/embed_dispatcher.h"
#include "daemon/write_queue.h"
#include "daemon/rpc.h"
#include "daemon/rpc_v2.h"
#include "config/config.h"
#include "core/version.h"
#include "storage/index_file.h"
#include "storage/glossary_store.h"
#include "storage/storage_backend.h"
#include "storage/sqlite_backend.h"
#include "adapters/mem_adapter.h"
#include "core/loc.h"
#include "core/searcher.h"
#include "core/indexer.h"
#include "core/operations.h"
#include "daemon/file_watcher.h"
#include "config/utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <memory>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <signal.h>
#include <thread>
#include <vector>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace fs = std::filesystem;

using std::cerr;
using std::cout;
using std::map;
using std::string;
using std::vector;

static std::atomic<bool> g_shutdown{false};
static void signal_handler(int) { g_shutdown.store(true); }

static string pid_path() {
  return (fs::path(std::getenv("HOME")) / ".gogol" / "serve.pid").string();
}

// Tracks connection handler threads so they can be cancelled + joined on
// shutdown (rather than detached + abandoned). Each handler runs its body then
// marks itself done; the accept thread reaps finished threads each tick and
// joins all remaining on shutdown. On shutdown, live client fds are
// shutdown(SHUT_RDWR) first to unblock any thread stuck in read().
struct HandlerRegistry {
  struct Entry { std::thread th; int fd; std::atomic<bool> done{false}; };
  std::mutex mtx;
  std::map<uint64_t, std::unique_ptr<Entry>> handlers;
  uint64_t next_id = 1;

  // Spawn a handler thread running fn(). fd is the client socket (for
  // cancellation). The thread sets its done flag on exit (an atomic — no lock,
  // so it can never deadlock against a reaper/shutdown that holds mtx while
  // deciding what to join). Only the accept thread (reap/shutdown_all) joins.
  void spawn(int fd, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mtx);
    uint64_t id = next_id++;
    auto e = std::make_unique<Entry>();
    e->fd = fd;
    Entry* ep = e.get();
    e->th = std::thread([ep, fn = std::move(fn)]() {
      fn();
      ep->done.store(true);
    });
    handlers[id] = std::move(e);
  }

  // Join + erase finished threads. Called from the accept thread each tick to
  // bound registry growth. Collects joinable entries under the lock, then joins
  // OUTSIDE the lock (join must never run while holding mtx).
  void reap() {
    std::vector<std::unique_ptr<Entry>> to_join;
    {
      std::lock_guard<std::mutex> lock(mtx);
      for (auto it = handlers.begin(); it != handlers.end();) {
        if (it->second->done.load()) {
          to_join.push_back(std::move(it->second));
          it = handlers.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& e : to_join)
      if (e->th.joinable()) e->th.join();
  }

  // Cancel + join all handlers. Called once on shutdown (accept thread).
  void shutdown_all() {
    std::vector<std::unique_ptr<Entry>> all;
    {
      std::lock_guard<std::mutex> lock(mtx);
      for (auto& [id, e] : handlers) all.push_back(std::move(e));
      handlers.clear();
    }
    for (auto& e : all)
      if (e->fd >= 0 && !e->done.load())
        ::shutdown(e->fd, SHUT_RDWR); // unblock a stuck read() (still-running only)
    for (auto& e : all)
      if (e->th.joinable()) e->th.join();
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mtx);
    return handlers.size();
  }
};

// All daemon state
struct DaemonState {
  EmbedDispatcher* dispatcher = nullptr;
  std::mutex mtx; // protects the index map (snapshot publish)
  std::atomic<bool> indexing{false};
  std::mutex name_mtx; // guards indexing_name (writer writes, connection threads read)
  std::string indexing_name;
  std::atomic<int> index_progress{0};  // entries processed
  std::atomic<int> index_total{0};     // total entries to process
  std::atomic<int64_t> index_start_ms{0}; // epoch ms when indexing started
  HandlerRegistry handlers; // tracked connection threads (cancel + join on shutdown)
  // Immutable snapshots published via copy-swap. Readers hold a shared_ptr and
  // read lock-free; the snapshot stays alive even if a writer swaps concurrently.
  map<string, std::shared_ptr<const Index>> indexes;
  map<string, IndexConfig> configs;
  // All index mutations run on this single writer thread. Serializing writes
  // makes mutable_copy/publish race-free (no lost updates) and makes the
  // progress fields above single-writer. Declared last so its worker thread is
  // joined (in ~WriteQueue) before the maps/dispatcher it touches are destroyed.
  WriteQueue writes;

  void load_all() {
    configs = load_config();
    for (auto& [name, cfg] : configs) {
      if (cfg.is_indexed()) {
        // SQLite-only: indexes already live in <index>.db. No startup
        // migration — open the backend and load directly.
        auto backend = open_backend(name);
        auto idx = std::make_shared<Index>(backend->load_index());
        idx->ensure_embeddings(); // materialize before publishing (snapshot is const)
        indexes[name] = std::move(idx);
      }
    }
  }

  // Grab a stable snapshot under lock (may be nullptr if index unknown).
  std::shared_ptr<const Index> snapshot(const string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = indexes.find(name);
    return it == indexes.end() ? nullptr : it->second;
  }

  // Deep-copy the current snapshot into a mutable Index for a writer to modify.
  // Returns an empty Index if none exists yet.
  std::shared_ptr<Index> mutable_copy(const string& name) {
    auto snap = snapshot(name);
    if (snap) return std::make_shared<Index>(*snap);
    return std::make_shared<Index>();
  }

  // Atomically publish new immutable snapshots.
  void publish(const string& name, std::shared_ptr<Index> idx) {
    std::lock_guard<std::mutex> lock(mtx);
    indexes[name] = std::move(idx);
  }

  void set_indexing_name(const string& n) {
    std::lock_guard<std::mutex> lock(name_mtx);
    indexing_name = n;
  }

  string get_indexing_name() {
    std::lock_guard<std::mutex> lock(name_mtx);
    return indexing_name;
  }

  string root_of(const string& name) {
    if (configs.count(name) && !configs[name].paths.empty())
      return configs[name].paths[0];
    return {};
  }

  void save_idx(const string& name, const Index& idx) {
    open_backend(name)->save_index(root_of(name), idx);
  }

  void save_idx_meta_only(const string& name, const Index& idx) {
    open_backend(name)->save_index_meta_only(root_of(name), idx);
  }
};

// --- Command handlers ---

static void handle_query(MsgReader &reader, MsgWriter &resp, DaemonState &st) {
  DBG("RPC: QUERY");
  QueryRequest req;
  reader.get_str(req.query);
  reader.get_str(req.index);
  uint32_t tf, tk, sl; uint8_t sc;
  reader.get_u32(tf); reader.get_u32(tk); reader.get_u32(sl); reader.get_u8(sc);
  req.type_filter = (int32_t)tf; req.top_k = tk; req.show_lines = sl; req.scores = (sc!=0);

  auto index_names = req.index.empty() ? vector<string>{} : split_csv(req.index);
  if (index_names.empty())
    for (auto& [name, cfg] : st.configs)
      if (cfg.is_indexed()) index_names.push_back(name);

  // Query-side glossary expansion (symmetric with index-side enrichment): if a
  // glossary term for a searched index appears in the query, append its
  // expansion so an abbreviation query (e.g. "CFC") also matches the concept it
  // denotes ("Customer Fulfillment Center ..."). Purely a query-string rewrite —
  // no index/embedding change. Case-insensitive whole-word-ish match; each term
  // expanded at most once. Original query text is preserved (expansion appended).
  std::string expanded = req.query;
  {
    std::string upper_q = req.query;
    std::transform(upper_q.begin(), upper_q.end(), upper_q.begin(), ::toupper);
    std::set<std::string> applied; // dedup across indexes
    for (auto& name : index_names) {
      auto git = st.configs.find(name);
      if (git == st.configs.end()) continue;
      auto glossary = open_backend(name)->load_glossary();
      for (auto& [term, expansion] : glossary) {
        if (term.empty() || applied.count(term)) continue;
        std::string upper_term = term;
        std::transform(upper_term.begin(), upper_term.end(), upper_term.begin(), ::toupper);
        // Match the term as a substring of the (upper-cased) query. Mirrors the
        // index-side enrich_embed_text matching. Guard against a term matching
        // inside a larger word by requiring word boundaries.
        auto pos = upper_q.find(upper_term);
        bool word_bounded = false;
        while (pos != std::string::npos) {
          bool left_ok = (pos == 0) || !std::isalnum((unsigned char)upper_q[pos - 1]);
          size_t end = pos + upper_term.size();
          bool right_ok = (end >= upper_q.size()) || !std::isalnum((unsigned char)upper_q[end]);
          if (left_ok && right_ok) { word_bounded = true; break; }
          pos = upper_q.find(upper_term, pos + 1);
        }
        if (word_bounded) {
          expanded += " " + expansion;
          applied.insert(term);
        }
      }
    }
  }

  SearchOptions opts{expanded, index_names, req.type_filter, req.top_k, req.show_lines};

  // Grab stable snapshots (embeddings already materialized at publish time).
  // Held for the duration of the search so a concurrent publish can't free them.
  std::vector<std::shared_ptr<const Index>> snaps;
  map<string, const Index*> index_ptrs;
  map<string, FtsKeywordFn> fts_providers;
  for (auto &name : index_names) {
    auto snap = st.snapshot(name);
    if (!snap) continue;
    index_ptrs[name] = snap.get();
    snaps.push_back(std::move(snap));
    // SQLite-backed index → keyword ranking comes from FTS5 MATCH on the DB.
    // Each provider opens a FRESH read-only connection on THIS handler thread
    // (WAL allows concurrent readers; Db is not shared across threads), so it
    // never touches the daemon's in-memory snapshots or the single-writer path.
    // Absent from the map ⇒ that index contributes cosine ranking only.
    // Query-only: no mutation, no reindex.
    if (is_sqlite_backed(name)) {
      fts_providers[name] =
          [name](const std::string &q, int limit)
              -> std::vector<std::pair<int, double>> {
        try {
          SqliteBackend be(name);
          return be.keyword_search_fts(q, limit);
        } catch (...) {
          // Any FTS/DB error → no keyword contribution for this index (cosine
          // still ranks). Never let it escape into the RPC dispatch.
          return {};
        }
      };
    }
  }

  // Query embedding goes through dispatcher (serialized with other embed work).
  // Cosine scan + keyword (FTS5) run on request thread against the immutable
  // snapshots.
  auto results = search(opts, *st.dispatcher, index_ptrs, st.configs,
                        fts_providers);

  resp.put_u8((uint8_t)RpcStatus::OK);
  resp.put_u32((uint32_t)results.size());
  for (auto& r : results) {
    resp.put_str(r.index);
    resp.put_str(r.path);
    resp.put_u32(r.line);
    resp.put_str(r.chunk);
    resp.put_u8(r.proto);
    resp.put_str(r.status);
    resp.put_float(r.score);
    resp.put_float(r.cosine);
    resp.put_str(r.snippet);
  }
}

static void handle_list(MsgReader &reader, MsgWriter &resp, DaemonState &st) {
  DBG("RPC: LIST");
  uint8_t has_type_b = 0, type_b = 0;
  reader.get_u8(has_type_b);
  reader.get_u8(type_b);
  string idx_name; reader.get_str(idx_name);
  bool has_type = (has_type_b != 0);
  EntryType want = (EntryType)type_b;

  // Collect entries
  vector<ListResultEntry> entries;

  if (!idx_name.empty()) {
    // File (doc) + memory (note) entries live in the index.
    bool want_docs = !has_type || want == EntryType::DOC;
    bool want_notes = !has_type || want == EntryType::NOTE;
    bool want_terms = !has_type || want == EntryType::TERM;

    if ((want_docs || want_notes)) {
      if (auto snap = st.snapshot(idx_name)) {
        for (auto& e : snap->entries) {
          bool is_mem = (e.proto == EntryType::NOTE);
          if (is_mem && !want_notes) continue;
          if (!is_mem && !want_docs) continue;
          ListResultEntry le;
          le.proto = (uint8_t)e.proto;
          le.index = idx_name;
          le.path = e.path;
          le.line = e.line;
          le.chunk = e.chunk;
          entries.push_back(std::move(le));
        }
      }
    }
    if (want_terms) {
      auto glossary = open_backend(idx_name)->load_glossary();
      for (auto& [term, expansion] : glossary) {
        ListResultEntry le;
        le.proto = 2; // GLOSSARY
        le.index = idx_name;
        le.path = term;
        entries.push_back(std::move(le));
      }
    }
  }

  resp.put_u8((uint8_t)RpcStatus::OK);
  resp.put_u32((uint32_t)entries.size());
  for (auto& e : entries) {
    resp.put_u8(e.proto);
    resp.put_str(e.index);
    resp.put_str(e.path);
    resp.put_u32(e.line);
    resp.put_str(e.chunk);
  }
}

static void handle_index(MsgReader &reader, MsgWriter &resp, DaemonState &st) {
  DBG("RPC: INDEX");
  IndexRequest req; reader.get_str(req.index); uint8_t f; reader.get_u8(f); req.force=(f&0x01)!=0;
  vector<string> to_index;
  if (!req.index.empty()) to_index.push_back(req.index);
  else for (auto& [name, cfg] : st.configs) if (cfg.is_indexed() && !cfg.paths.empty()) to_index.push_back(name);

  // If already indexing, report that
  if (st.indexing.load()) {
    resp.put_u8((uint8_t)RpcStatus::OK);
    resp.put_str("Already indexing: " + st.get_indexing_name() + "\n");
    return;
  }

  // Always index on the writer thread — never block the accept loop, and
  // serialize with all other mutations. Progress is observable via STATUS.
  for (auto& name : to_index) {
    st.writes.submit([&st, name, force = req.force]() {
      if (!st.configs.count(name)) return;
      try {
        st.set_indexing_name(name);
        st.index_progress.store(0);
        st.index_total.store(0);
        st.index_start_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        st.indexing.store(true);

        // Work on a mutable deep copy of the current snapshot, then publish.
        auto idx_copy = st.mutable_copy(name);

        // Embedding goes through the dispatcher (serialized with query/add)
        run_index(name, st.configs[name], *st.dispatcher, *idx_copy, force, "",
                  [&st](int current, int total) {
                    st.index_progress.store(current);
                    st.index_total.store(total);
                  });

        // Materialize embeddings before publishing the const snapshot
        idx_copy->ensure_embeddings();
        st.publish(name, std::move(idx_copy));
        st.indexing.store(false);
      } catch (const std::exception& ex) {
        st.indexing.store(false);
        fprintf(stderr, "[index] %s failed: %s\n", name.c_str(), ex.what());
      } catch (...) {
        st.indexing.store(false);
        fprintf(stderr, "[index] %s failed: unknown\n", name.c_str());
      }
    });
  }
  string msg = "Indexing started";
  if (to_index.size() == 1) msg += ": " + to_index[0];
  else msg += " (" + std::to_string(to_index.size()) + " indexes)";
  resp.put_u8((uint8_t)RpcStatus::OK); resp.put_str(msg + "\n");
}

static void handle_status(MsgReader &reader, MsgWriter &resp, DaemonState &st) {
  DBG("RPC: STATUS");
  string index; reader.get_str(index);
  // Return indexing state + per-index counts
  resp.put_u8((uint8_t)RpcStatus::OK);
  resp.put_u8(st.indexing.load() ? 1 : 0);
  resp.put_str(st.get_indexing_name());
  resp.put_u32(st.index_progress.load());
  resp.put_u32(st.index_total.load());
  int64_t elapsed_ms = 0;
  if (st.indexing.load() && st.index_start_ms.load() > 0) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    elapsed_ms = now - st.index_start_ms.load();
  }
  resp.put_u32((uint32_t)(elapsed_ms / 1000)); // elapsed seconds
  vector<string> names;
  if (!index.empty()) names.push_back(index);
  else for (auto& [name, cfg] : st.configs) if (cfg.is_indexed()) names.push_back(name);
  resp.put_u32((uint32_t)names.size());
  for (auto& name : names) {
    int fc=0, mc=0; uint32_t dim=0;
    if (auto snap = st.snapshot(name)) { dim=snap->dim; for (auto& e : snap->entries) { if (e.proto==EntryType::DOC) fc++; else mc++; } }
    resp.put_str(name);
    resp.put_u32(fc);
    resp.put_u32(mc);
    resp.put_u32(dim);
  }
}

// --- Connection wrapper (plain or encrypted) ---

struct Connection {
  int fd;
  CryptoState crypto;
  bool encrypted = false;

  bool write_msg(const uint8_t *data, uint32_t len) {
    if (encrypted) return crypto_frame_write(fd, crypto, data, len);
    return frame_write(fd, data, len);
  }

  vector<uint8_t> read_msg() {
    if (encrypted) return crypto_frame_read(fd, crypto);
    return frame_read(fd);
  }
};

// --- Client dispatch ---

static void handle_client_conn(Connection& conn, DaemonState& st) {
  while (!g_shutdown.load()) {
    auto msg = conn.read_msg();
    if (msg.empty()) break;

    MsgReader reader(std::move(msg));
    uint8_t cmd;
    if (!reader.get_u8(cmd)) break;

    // For each command, we need to pass the remaining reader data to handlers
    // But handlers currently use raw fd reads. We need to refactor them.
    // Simpler approach: handlers write response to a MsgWriter, then we send it.

    MsgWriter resp;

    // STATUS/PING/SHUTDOWN don't need the lock
    if (static_cast<RpcCmd>(cmd) == RpcCmd::STATUS) {
      handle_status(reader, resp, st);
      conn.write_msg(resp.data().data(), (uint32_t)resp.data().size());
      continue;
    }
    if (static_cast<RpcCmd>(cmd) == RpcCmd::PING) {
      resp.put_u8((uint8_t)RpcStatus::OK);
      resp.put_str(GOGOL_BUILD_VERSION);
      conn.write_msg(resp.data().data(), (uint32_t)resp.data().size());
      continue;
    }
    if (static_cast<RpcCmd>(cmd) == RpcCmd::SHUTDOWN) {
      resp.put_u8((uint8_t)RpcStatus::OK);
      resp.put_str("bye");
      conn.write_msg(resp.data().data(), (uint32_t)resp.data().size());
      g_shutdown.store(true);
      break;
    }

    // Handlers manage their own locking via DaemonState snapshot/publish.
    // Readers grab a stable snapshot; writers copy-swap. No coarse lock here
    // (a coarse lock previously caused an RM self-deadlock and serialized all
    // clients).

    // Guard every handler: an uncaught exception (e.g. filesystem_error from a
    // bad path, bad_alloc) would otherwise hit llama.cpp's global terminate
    // handler and abort() the whole daemon. Convert to an error response.
    try {
    switch (static_cast<RpcCmd>(cmd)) {
    case RpcCmd::QUERY: {
      handle_query(reader, resp, st);
      break;
    }
    case RpcCmd::ADD: {
      AddRequest req;
      uint8_t type_b = 0;
      reader.get_u8(type_b);
      req.type = (EntryType)type_b;
      reader.get_str(req.index);
      reader.get_str(req.path);
      reader.get_str(req.content);
      reader.get_str(req.sources);

      string name = req.index;
      if (name.empty()) { resp.put_u8((uint8_t)RpcStatus::ERR); resp.put_str("No index specified"); break; }

      // All adds delegate to op_add on the writer thread (serialized with every
      // other mutation) and wait for the result so the client gets an accurate
      // ack. op_add routes by EntryType: term -> glossary, note -> memory-dir
      // note file (always writable), doc -> code file (rw only). It writes the
      // backing file/store AND the index entry together, so an entry can never
      // exist without its content.
      {
        IndexConfig cfg;
        if (st.configs.count(name)) cfg = st.configs[name];
        auto result = st.writes.submit_wait<OpResult>([&st, req, cfg, name]() {
          auto idx_copy = st.mutable_copy(name);
          auto r = op_add(req.type, req.index, req.path, req.content, req.sources,
                          cfg, *st.dispatcher, *idx_copy);
          if (r.ok) {
            idx_copy->ensure_embeddings();
            // Terms don't change the index entries; others need meta saved.
            if (req.type != EntryType::TERM) st.save_idx_meta_only(name, *idx_copy);
            st.publish(name, std::move(idx_copy));
          }
          return r;
        });
        resp.put_u8(result.ok ? (uint8_t)RpcStatus::OK : (uint8_t)RpcStatus::ERR);
        resp.put_str(result.message);
      }
      break;
    }
    case RpcCmd::RM: {
      uint8_t type_b = 0; reader.get_u8(type_b);
      EntryType type = (EntryType)type_b;
      string name; reader.get_str(name);
      string path; reader.get_str(path);
      if (name.empty()) { resp.put_u8((uint8_t)RpcStatus::ERR); resp.put_str("No index specified"); break; }

      IndexConfig cfg;
      if (st.configs.count(name)) cfg = st.configs[name];

      {
        // Run on the writer thread (serialized with all mutations); wait for
        // the result since the client expects a synchronous ack.
        auto result = st.writes.submit_wait<OpResult>([&st, type, name, path, cfg]() {
          auto idx_copy = st.mutable_copy(name);
          auto r = op_rm(type, name, path, cfg, *idx_copy);
          if (r.ok) {
            // Note entries: only .meta needs update (no .emb rewrite).
            if (type == EntryType::NOTE)
              st.save_idx_meta_only(name, *idx_copy);
            else if (type != EntryType::TERM)
              st.save_idx(name, *idx_copy);
            idx_copy->ensure_embeddings();
            st.publish(name, std::move(idx_copy));
          }
          return r;
        });
        resp.put_u8(result.ok ? (uint8_t)RpcStatus::OK : (uint8_t)RpcStatus::ERR);
        resp.put_str(result.message);
      }
      break;
    }
    case RpcCmd::GET: {
      uint8_t type_b = 0; reader.get_u8(type_b);
      EntryType type = (EntryType)type_b;
      string name; reader.get_str(name);
      string path; reader.get_str(path);
      uint32_t max_lines = 0; reader.get_u32(max_lines);
      if (name.empty()) { resp.put_u8((uint8_t)RpcStatus::ERR); resp.put_str("No index specified"); break; }

      IndexConfig cfg;
      if (st.configs.count(name)) cfg = st.configs[name];

      auto result = op_get(type, name, path, cfg, (int)max_lines);
      resp.put_u8(result.ok ? (uint8_t)RpcStatus::OK : (uint8_t)RpcStatus::ERR);
      resp.put_str(result.message);
      break;
    }
    case RpcCmd::LIST: {
      handle_list(reader, resp, st);
      break;
    }
    case RpcCmd::INDEX: {
      handle_index(reader, resp, st);
      break;
    }
    default:
      resp.put_u8((uint8_t)RpcStatus::ERR); resp.put_str("Unknown command");
      break;
    }
    } catch (const std::exception &ex) {
      fprintf(stderr, "[rpc] handler exception: %s\n", ex.what());
      resp.clear();
      resp.put_u8((uint8_t)RpcStatus::ERR);
      resp.put_str(string("Internal error: ") + ex.what());
    } catch (...) {
      fprintf(stderr, "[rpc] handler exception: unknown\n");
      resp.clear();
      resp.put_u8((uint8_t)RpcStatus::ERR);
      resp.put_str("Internal error");
    }

    conn.write_msg(resp.data().data(), (uint32_t)resp.data().size());
    if (g_shutdown.load()) break;
  }
  close(conn.fd);
}

// Unix socket handler — framed, no encryption
static void handle_client(int client_fd, DaemonState& st) {
  Connection conn;
  conn.fd = client_fd;
  conn.encrypted = false;
  handle_client_conn(conn, st);
}

// --- Daemonize: fork + exec self with --foreground ---

static int daemonize(const string &pid_file, const string &tcp_addr) {
  pid_t pid = fork();
  if (pid < 0) { cerr << "Fork failed.\n"; return -1; }
  if (pid > 0) {
    // Parent: wait for daemon to be ready (socket appears) before reporting
    std::ofstream pf(pid_file);
    pf << pid << "\n";
    string sock = daemon_socket_path();
    for (int i = 0; i < 100; i++) { // up to 10s
      if (fs::exists(sock)) {
        cout << "Daemon started (PID " << pid << ")\n";
        return 0;
      }
      usleep(100000); // 100ms
    }
    cout << "Daemon started (PID " << pid << ") — still loading model\n";
    return 0; // parent exits
  }
  // Child: detach, redirect IO, exec self
  setsid();
  string log_path = (fs::path(std::getenv("HOME")) / ".gogol" / "serve.log").string();
  int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (log_fd >= 0) {
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
  }
  close(STDIN_FILENO);

  // Resolve own executable path
  string self;
#ifdef __APPLE__
  char path_buf[4096];
  uint32_t path_size = sizeof(path_buf);
  if (_NSGetExecutablePath(path_buf, &path_size) == 0)
    self = fs::canonical(path_buf).string();
#else
  self = fs::read_symlink("/proc/self/exe").string();
#endif

  vector<string> exec_args = {self, "serve", "--foreground"};
  if (!tcp_addr.empty()) { exec_args.push_back("--tcp"); exec_args.push_back(tcp_addr); }

  vector<char*> argv_ptrs;
  for (auto& a : exec_args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  argv_ptrs.push_back(nullptr);

  execv(self.c_str(), argv_ptrs.data());
  perror("execv");
  return 1; // exec failed, fall through
}

// --- Setup TCP socket ---

static int setup_tcp_socket(const string &tcp_addr) {
  auto colon = tcp_addr.rfind(':');
  string host = (colon != string::npos) ? tcp_addr.substr(0, colon) : "127.0.0.1";
  int port = (colon != string::npos) ? std::atoi(tcp_addr.substr(colon + 1).c_str()) : 9400;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in taddr{};
  taddr.sin_family = AF_INET;
  taddr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &taddr.sin_addr);

  if (bind(fd, (struct sockaddr *)&taddr, sizeof(taddr)) != 0) {
    cerr << "Cannot bind TCP " << tcp_addr << "\n"; close(fd); return -1;
  }
  if (listen(fd, 128) != 0) {
    cerr << "Cannot listen on TCP.\n"; close(fd); return -1;
  }
  return fd;
}

// --- Public API ---

int run_embed_server(const string &model_path, bool foreground,
                     const string &tcp_addr) {
  string sock_path = daemon_socket_path();
  string pid_file = pid_path();

  if (!foreground && daemon_is_running()) {
    cerr << "Daemon already running.\n";
    return 1;
  }

  if (!foreground) {
    int rc = daemonize(pid_file, tcp_addr);
    if (rc <= 0) return rc == 0 ? 0 : 1; // parent exits or exec failed
    // If we get here, exec failed — fall through to run in-process
  } else {
    std::ofstream pf(pid_file);
    pf << getpid() << "\n";
  }

  signal(SIGPIPE, SIG_IGN); // prevent crash when client disconnects mid-write
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);
  unlink(sock_path.c_str());
  // Clean stale indexing indicator from previous crash
  unlink((fs::path(std::getenv("HOME")) / ".gogol" / "indexing").string().c_str());

  // Unix socket
  int unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (unix_fd < 0) { cerr << "Cannot create unix socket.\n"; return 1; }

  struct sockaddr_un uaddr{};
  uaddr.sun_family = AF_UNIX;
  strncpy(uaddr.sun_path, sock_path.c_str(), sizeof(uaddr.sun_path) - 1);

  if (bind(unix_fd, (struct sockaddr *)&uaddr, sizeof(uaddr)) != 0) {
    cerr << "Cannot bind unix socket.\n"; close(unix_fd); return 1;
  }
  if (listen(unix_fd, 128) != 0) {
    cerr << "Cannot listen on unix socket.\n"; close(unix_fd); return 1;
  }

  // TCP socket (optional)
  int tcp_fd = -1;
  if (!tcp_addr.empty()) {
    tcp_fd = setup_tcp_socket(tcp_addr);
    if (tcp_fd < 0) { close(unix_fd); return 1; }
  }

  // Load model + state
  fprintf(stderr, "Loading model: %s\n", model_path.c_str());
  Embedder embedder(model_path);
  EmbedDispatcher dispatcher(embedder);
  DaemonState st;
  st.dispatcher = &dispatcher;
  st.load_all();
  fprintf(stderr, "Ready (dim=%d, %zu indexes). Unix: %s",
          embedder.dim(), st.indexes.size(), sock_path.c_str());
  if (tcp_fd >= 0) fprintf(stderr, " TCP: %s", tcp_addr.c_str());
  fprintf(stderr, "\n");

  // File watcher: auto-reindex on filesystem changes. Start it if watching is
  // effectively enabled for ANY index — the global default OR a per-index
  // override can turn it on. Per-index enable/debounce is resolved inside
  // start_file_watcher via effective_watch().
  auto gc = load_global_config();
  bool any_watch = gc.watch;
  if (!any_watch) {
    for (auto &[nm, cfg] : st.configs)
      if (effective_watch(cfg, gc)) { any_watch = true; break; }
  }
  if (any_watch) {
    auto reindex_cb = [](const string &idx_name, void *ctx) {
      auto *state = static_cast<DaemonState *>(ctx);
      if (!state->configs.count(idx_name)) return;
      fprintf(stderr, "[watch] Reindexing %s...\n", idx_name.c_str());

      // Enqueue onto the writer thread — the watcher thread does no index work
      // itself, so it can't race with adds/rm/index or block on embedding.
      state->writes.submit([state, idx_name]() {
        try {
          state->set_indexing_name(idx_name);
          state->indexing.store(true);
          { string ipath = (fs::path(std::getenv("HOME")) / ".gogol" / "indexing").string();
            std::ofstream(ipath) << idx_name << "\n"; }
          auto idx_copy = state->mutable_copy(idx_name);
          auto result = run_index(idx_name, state->configs[idx_name], *state->dispatcher,
                                  *idx_copy, false, "");
          idx_copy->ensure_embeddings();
          state->publish(idx_name, std::move(idx_copy));
          state->indexing.store(false);
          unlink((fs::path(std::getenv("HOME")) / ".gogol" / "indexing").string().c_str());
          fprintf(stderr, "[watch] %s: %d updated, %d total\n",
                  idx_name.c_str(), result.embedded, result.total);
        } catch (const std::exception &ex) {
          state->indexing.store(false);
          fprintf(stderr, "[watch] reindex %s failed: %s\n", idx_name.c_str(), ex.what());
        } catch (...) {
          state->indexing.store(false);
          fprintf(stderr, "[watch] reindex %s failed: unknown\n", idx_name.c_str());
        }
      });
    };
    if (start_file_watcher(gc.watch_debounce_ms, reindex_cb, &st))
      fprintf(stderr, "File watcher enabled (debounce=%dms)\n", gc.watch_debounce_ms);
    else
      fprintf(stderr, "File watcher failed to start\n");
  }

  // Accept loop using select() on both fds
  while (!g_shutdown.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(unix_fd, &fds);
    int max_fd = unix_fd;
    if (tcp_fd >= 0) {
      FD_SET(tcp_fd, &fds);
      if (tcp_fd > max_fd) max_fd = tcp_fd;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    int ready = select(max_fd + 1, &fds, nullptr, nullptr, &tv);
    // Reap finished handler threads each tick to bound registry growth.
    st.handlers.reap();
    if (ready <= 0) continue;

    if (FD_ISSET(unix_fd, &fds)) {
      int client_fd = accept(unix_fd, nullptr, nullptr);
      if (client_fd >= 0) {
        // Thread-per-connection: a handler blocking on submit_wait (rm/glossary)
        // or a slow query no longer stalls accept() or other clients. Safe
        // because reads use immutable snapshots and writes are single-writer.
        // Tracked (not detached) so we can cancel + join on shutdown.
        st.handlers.spawn(client_fd, [client_fd, &st]() {
          handle_client(client_fd, st);
        });
      }
    }
    if (tcp_fd >= 0 && FD_ISSET(tcp_fd, &fds)) {
      int client_fd = accept(tcp_fd, nullptr, nullptr);
      if (client_fd >= 0) {
        // Handshake on the connection thread too (avoids blocking accept()).
        st.handlers.spawn(client_fd, [client_fd, &st]() {
          auto keys = load_keys();
          auto hs = server_handshake(client_fd, keys);
          if (hs.ok) {
            Connection conn;
            conn.fd = client_fd;
            conn.crypto = hs.crypto;
            conn.encrypted = hs.encrypted;
            handle_client_conn(conn, st);
          } else {
            close(client_fd);
          }
        });
      }
    }
  }

  stop_file_watcher();
  close(unix_fd);
  if (tcp_fd >= 0) close(tcp_fd);

  // Stop accepting, then cancel + join all in-flight connection handlers before
  // tearing down `st` — handler threads read `st`, so returning while any are
  // live would be a use-after-free. shutdown(SHUT_RDWR) on each client fd
  // unblocks any handler stuck in read() so the join always completes.
  st.handlers.shutdown_all();

  unlink(sock_path.c_str());
  unlink(pid_file.c_str());
  unlink((fs::path(std::getenv("HOME")) / ".gogol" / "indexing").string().c_str());
  fprintf(stderr, "Daemon stopped.\n");
  return 0;
}

bool stop_embed_server() {
  string sock_path = daemon_socket_path();
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd); cerr << "Daemon not running.\n"; return false;
  }

  // Send framed shutdown command
  MsgWriter msg;
  msg.put_u8((uint8_t)RpcCmd::SHUTDOWN);
  frame_write(fd, msg.data());
  // Read response (ignore content)
  frame_read(fd);
  close(fd);

  string pf = pid_path();
  for (int i = 0; i < 20 && fs::exists(pf); i++) usleep(100000);
  cout << "Daemon stopped.\n";
  return true;
}

int server_status() {
  if (!daemon_is_running()) {
    cout << "Status: stopped\n";
    return 0;
  }
  string pf = pid_path();
  string pid_str;
  if (fs::exists(pf)) { std::ifstream f(pf); std::getline(f, pid_str); }
  cout << "Status: running\n";
  if (!pid_str.empty()) cout << "PID: " << pid_str << "\n";
  cout << "Socket: " << daemon_socket_path() << "\n";
  return 0;
}
