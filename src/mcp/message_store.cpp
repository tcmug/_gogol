#include "mcp/message_store.h"

namespace gogol {
namespace mcp {

namespace {
constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS messages(
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,
  to_agent      TEXT NOT NULL,
  from_agent    TEXT NOT NULL,
  body          TEXT NOT NULL,
  ts            INTEGER NOT NULL,
  state         TEXT NOT NULL DEFAULT 'queued'
                CHECK(state IN ('queued','claimed','delivered')),
  claim_session TEXT,
  claimed_at    INTEGER,
  delivered_at  INTEGER
);
CREATE INDEX IF NOT EXISTS messages_inbox ON messages(to_agent, state, seq);
)SQL";

int changes(Db& db) { return sqlite3_changes(db.raw()); }
}  // namespace

MessageStore::MessageStore(const std::string& path) : db_(path) { db_.exec(kSchema); }

int64_t MessageStore::enqueue(const std::string& to, const std::string& from,
                              const std::string& body, int64_t ts) {
    Db::Stmt s(db_,
               "INSERT INTO messages(to_agent, from_agent, body, ts) VALUES(?,?,?,?)");
    s.bind(1, to);
    s.bind(2, from);
    s.bind(3, body);
    s.bind(4, ts);
    s.step();
    return sqlite3_last_insert_rowid(db_.raw());
}

std::optional<StoredMsg> MessageStore::claim_next(const std::string& to,
                                                  const std::string& session, int64_t now) {
    Db::Tx tx(db_);
    StoredMsg m;
    {
        Db::Stmt s(db_,
                   "SELECT seq, from_agent, body, ts FROM messages "
                   "WHERE to_agent = ? AND state = 'queued' ORDER BY seq LIMIT 1");
        s.bind(1, to);
        if (!s.step()) return std::nullopt;  // Tx rolls back (nothing written)
        m.seq = s.column_int64(0);
        m.from = s.column_text(1);
        m.body = s.column_text(2);
        m.ts = s.column_int64(3);
        m.to = to;
    }
    if (session.empty()) {
        Db::Stmt u(db_,
                   "UPDATE messages SET state = 'delivered', delivered_at = ? WHERE seq = ?");
        u.bind(1, now);
        u.bind(2, m.seq);
        u.step();
    } else {
        Db::Stmt u(db_,
                   "UPDATE messages SET state = 'claimed', claim_session = ?, claimed_at = ? "
                   "WHERE seq = ?");
        u.bind(1, session);
        u.bind(2, now);
        u.bind(3, m.seq);
        u.step();
    }
    tx.commit();
    return m;
}

int MessageStore::ack(const std::string& to, const std::vector<int64_t>& seqs, int64_t now) {
    Db::Tx tx(db_);
    Db::Stmt u(db_,
               "UPDATE messages SET state = 'delivered', delivered_at = ? "
               "WHERE seq = ? AND to_agent = ? AND state != 'delivered'");
    int n = 0;
    for (int64_t seq : seqs) {
        u.reset();
        u.bind(1, now);
        u.bind(2, seq);
        u.bind(3, to);
        u.step();
        n += changes(db_);
    }
    tx.commit();
    return n;
}

int MessageStore::requeue_other_sessions(const std::string& to,
                                         const std::string& keep_session) {
    Db::Stmt u(db_,
               "UPDATE messages SET state = 'queued', claim_session = NULL, claimed_at = NULL "
               "WHERE to_agent = ? AND state = 'claimed' AND claim_session != ?");
    u.bind(1, to);
    u.bind(2, keep_session);
    u.step();
    return changes(db_);
}

int MessageStore::requeue_all() {
    db_.exec(
        "UPDATE messages SET state = 'queued', claim_session = NULL, claimed_at = NULL "
        "WHERE state = 'claimed'");
    return changes(db_);
}

int MessageStore::requeue_stale(int64_t claimed_before) {
    Db::Stmt u(db_,
               "UPDATE messages SET state = 'queued', claim_session = NULL, claimed_at = NULL "
               "WHERE state = 'claimed' AND claimed_at < ?");
    u.bind(1, claimed_before);
    u.step();
    return changes(db_);
}

int MessageStore::prune(int64_t delivered_before) {
    Db::Stmt d(db_, "DELETE FROM messages WHERE state = 'delivered' AND delivered_at < ?");
    d.bind(1, delivered_before);
    d.step();
    return changes(db_);
}

std::vector<StoredMsg> MessageStore::queued(const std::string& to) {
    Db::Stmt s(db_,
               "SELECT seq, from_agent, body, ts FROM messages "
               "WHERE to_agent = ? AND state = 'queued' ORDER BY seq");
    s.bind(1, to);
    std::vector<StoredMsg> out;
    while (s.step()) {
        StoredMsg m;
        m.seq = s.column_int64(0);
        m.from = s.column_text(1);
        m.body = s.column_text(2);
        m.ts = s.column_int64(3);
        m.to = to;
        out.push_back(std::move(m));
    }
    return out;
}

int MessageStore::count_queued(const std::string& to) {
    Db::Stmt s(db_, "SELECT COUNT(*) FROM messages WHERE to_agent = ? AND state = 'queued'");
    s.bind(1, to);
    s.step();
    return static_cast<int>(s.column_int64(0));
}

std::vector<std::pair<std::string, int>> MessageStore::queued_counts() {
    Db::Stmt s(db_,
               "SELECT to_agent, COUNT(*) FROM messages WHERE state = 'queued' "
               "GROUP BY to_agent");
    std::vector<std::pair<std::string, int>> out;
    while (s.step()) out.emplace_back(s.column_text(0), static_cast<int>(s.column_int64(1)));
    return out;
}

int MessageStore::trim_queued(const std::string& to, int cap) {
    Db::Stmt d(db_,
               "DELETE FROM messages WHERE seq IN ("
               " SELECT seq FROM messages WHERE to_agent = ? AND state = 'queued'"
               " ORDER BY seq DESC LIMIT -1 OFFSET ?)");
    d.bind(1, to);
    d.bind(2, static_cast<int64_t>(cap < 0 ? 0 : cap));
    d.step();
    return changes(db_);
}

int64_t MessageStore::max_seq() {
    Db::Stmt s(db_, "SELECT seq FROM sqlite_sequence WHERE name = 'messages'");
    return s.step() ? s.column_int64(0) : 0;
}

}  // namespace mcp
}  // namespace gogol
