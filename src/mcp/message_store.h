// Persistent queue behind the combadge message bus (ADR-003).
//
// One SQLite file (gogol_home()/messages.db, WAL) holds every message. Only the daemon
// opens it, and the daemon is a flock singleton, so there is exactly one writer.
//
// Row lifecycle:  queued --claim_next(session)--> claimed --ack--> delivered
//                 queued --claim_next("")-------> delivered   (consume-on-receive)
//                 claimed --requeue_*-----------> queued      (same seq, redelivered)
// Delivered rows stay as an analysis log until prune() removes them.
//
// seq is an AUTOINCREMENT rowid, so it is never reused, even after prune or a reopen.
//
// Not thread-safe: the caller (MessageBus) serializes every call under its own mutex.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "storage/db.h"

namespace gogol {
namespace mcp {

struct StoredMsg {
    int64_t seq = 0;
    std::string to;
    std::string from;
    std::string body;
    int64_t ts = 0;  // unix seconds, send time
};

class MessageStore {
public:
    explicit MessageStore(const std::string& path);

    // Append a queued message. Returns its seq.
    int64_t enqueue(const std::string& to, const std::string& from, const std::string& body,
                    int64_t ts);

    // Take the oldest queued message for `to`. With a session id the row becomes claimed
    // by that session (needs an ack); with an empty session it becomes delivered at once.
    std::optional<StoredMsg> claim_next(const std::string& to, const std::string& session,
                                        int64_t now);

    // Mark claimed (or still queued) messages of `to` delivered. Returns rows changed.
    int ack(const std::string& to, const std::vector<int64_t>& seqs, int64_t now);

    // Requeue `to`'s claims held by any session other than keep_session.
    int requeue_other_sessions(const std::string& to, const std::string& keep_session);
    // Requeue every claim (daemon start: no session survives a restart).
    int requeue_all();
    // Requeue claims made before `claimed_before` (safety lease for vanished sessions).
    int requeue_stale(int64_t claimed_before);
    // Delete delivered rows delivered before `delivered_before`.
    int prune(int64_t delivered_before);

    // Queued (not claimed, not delivered) messages for `to`, oldest first.
    std::vector<StoredMsg> queued(const std::string& to);
    int count_queued(const std::string& to);
    // Queued count per recipient (only recipients with at least one).
    std::vector<std::pair<std::string, int>> queued_counts();
    // Delete `to`'s oldest queued rows so at most `cap` remain. Returns rows dropped.
    int trim_queued(const std::string& to, int cap);
    // Highest seq ever assigned (0 if none). Survives prune via sqlite_sequence.
    int64_t max_seq();

private:
    Db db_;
};

}  // namespace mcp
}  // namespace gogol
