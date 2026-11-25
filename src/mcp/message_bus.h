// Agent message bus — the shared rendezvous for combadge.
//
// A single process (gogol serve) owns the bus. Agents (coding-agent sessions, host or container)
// are thin clients: they send/receive over the unix socket or POST /mcp.
// Messages live in a MessageStore (SQLite, ADR-003), so they survive a daemon restart.
// Wakeups, presence and per-agent counters stay in RAM.
//
// Design:
//   - one FIFO inbox per recipient name in the store, capped (drop-oldest queued).
//   - receive blocks on a per-name condition_variable until a send wakes it or the cap
//     elapses. gogol serve is thread-per-connection, so a blocked receive parks its own
//     thread — no event loop, no waiter-queue bookkeeping.
//   - send returns delivered_live (a receiver was blocked waiting), queued_depth, and
//     your_inbox (the sender's own unread count — a glare signal: "you have unread too").
//   - presence: set_online/is_online track which names are currently listening (a fresh
//     marker), so send can report delivered_live even when no receive is parked yet.
//
// Thread-safety: ALL state, including every MessageStore call, is guarded by one mutex. receive uses a predicate wait so a
// spurious wake or a message stolen by another thread re-blocks correctly.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/message_store.h"

namespace gogol {
namespace mcp {

struct BusMsg {
    int64_t seq = 0;
    std::string from;
    std::string body;
    long ts = 0;  // unix seconds
};

struct SendOutcome {
    int64_t seq = 0;
    bool delivered_live = false;  // a receiver was blocked waiting, OR the target is online
    int queued_depth = 0;         // recipient inbox depth after this send
    int your_inbox = 0;           // sender's own unread count (glare signal)
};

struct ReceiveOutcome {
    std::vector<BusMsg> messages;  // one (consume) or all (peek-mode); empty on timeout
    bool timed_out = false;
};

// Per-agent observability row (for the status page).
struct AgentStat {
    std::string name;
    bool online = false;    // fresh presence marker
    int inbox_depth = 0;    // currently-queued messages addressed to this agent
    long sent = 0;          // messages this agent has sent (lifetime of the process)
    long received = 0;      // messages this agent has consumed via receive
    long last_seen = 0;     // unix seconds of its last send/receive/presence activity
};

struct BusStats {
    int agents = 0;               // distinct names seen (sent/received/online/queued)
    int online = 0;               // currently-listening agents
    long total_sent = 0;          // seq high-water mark (persistent across restarts)
    int queued = 0;               // messages currently sitting in all inboxes
    std::vector<AgentStat> rows;  // per-agent, sorted by name
};

// One instance is shared across all serve threads.
class MessageBus {
public:
    // db_path: the messages.db file. The default ":memory:" gives a private,
    // non-persistent store (tests).
    explicit MessageBus(const std::string& db_path = ":memory:", size_t inbox_cap = 1000)
        : store_(db_path), inbox_cap_(inbox_cap) {}
    explicit MessageBus(size_t inbox_cap) : MessageBus(":memory:", inbox_cap) {}

    // Return every claimed message to queued. Call once at daemon start: no listen
    // session survives a restart, so their unacked claims must be redelivered.
    int requeue_all_claims();

    // Enqueue a message for `to` from `from`. Wakes one blocked receiver if any.
    SendOutcome send(const std::string& to, const std::string& from, const std::string& body);

    // Block up to timeout_ms for a message addressed to `me`. consume=true removes and
    // returns the oldest one; consume=false returns a snapshot of all queued (blocking
    // peek). timeout_ms<=0 or >cap is clamped to cap_ms. Returns timed_out=true if none.
    //
    // session: the caller's listen-session id. Empty = consume-on-receive (delivered at
    // once, no ack). Non-empty = the message is claimed by this session and stays
    // redeliverable until ack(); claims held by any OTHER session of `me` are requeued
    // first, because a new listen session means the old one is gone (ADR-003).
    ReceiveOutcome receive(const std::string& me, long timeout_ms, bool consume,
                           long cap_ms = 300000, const std::string& session = "");

    // Retention (ADR-003): requeue claims older than kClaimLease (their session vanished
    // without a restart) and prune delivered rows older than kDeliveredKeep. Wakes
    // receivers if anything was requeued. `now` is unix seconds.
    struct SweepResult { int requeued = 0; int pruned = 0; };
    SweepResult sweep(long now);
    static constexpr long kClaimLease = 3600;               // 1h
    static constexpr long kDeliveredKeep = 30L * 24 * 3600;  // 30 days

    // Unread hint for `me` (messaging over MCP): the queued count and distinct senders,
    // returned only when it differs from the last hint given for `me` (count or newest
    // seq changed), so repeated tool calls don't repeat it. A zero count resets, so the
    // next message hints again.
    struct UnreadHint { int count = 0; std::vector<std::string> from; };
    std::optional<UnreadHint> unread_hint(const std::string& me);

    // Confirm claimed messages of `me` were handled. Returns how many were newly acked.
    int ack(const std::string& me, const std::vector<int64_t>& seqs);

    // Non-blocking snapshot of `me`'s inbox (does not consume).
    ReceiveOutcome peek(const std::string& me);

    // Presence: mark `name` as listening (fresh) or not.
    void set_online(const std::string& name, bool on);
    bool is_online(const std::string& name) const;

    // Observability snapshot for the status page: agents, online count, totals, per-agent.
    BusStats stats() const;

private:
    // RAM-only wakeup state per recipient; the messages themselves are in store_.
    struct Inbox {
        std::condition_variable cv;
        int waiters = 0;  // receivers currently blocked on cv
        // Newest listen session seen for this name. A parked receive from an older
        // session (its client aborted the poll, e.g. on /reload, but the server thread is
        // still waiting) must not claim messages: it would hand them to a dead socket.
        std::string live_session;
    };

    // Per-agent lifetime counters (seen the moment a name sends/receives).
    struct AgentCounters {
        long sent = 0;
        long received = 0;
        long last_seen = 0;  // unix seconds
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Inbox> inboxes_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> online_;
    std::unordered_map<std::string, AgentCounters> counters_;
    // Last unread hint per name: (count, newest seq). See unread_hint.
    std::unordered_map<std::string, std::pair<int, int64_t>> last_hint_;
    mutable MessageStore store_;
    size_t inbox_cap_;

    static constexpr std::chrono::seconds kOnlineFresh{90};
};

}  // namespace mcp
}  // namespace gogol
