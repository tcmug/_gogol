#include "mcp/message_bus.h"

#include <algorithm>

namespace gogol {
namespace mcp {

namespace {
long now_secs() {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
}

BusMsg to_bus_msg(StoredMsg&& m) {
    BusMsg b;
    b.seq = m.seq;
    b.from = std::move(m.from);
    b.body = std::move(m.body);
    b.ts = static_cast<long>(m.ts);
    return b;
}
}  // namespace

int MessageBus::requeue_all_claims() {
    std::lock_guard<std::mutex> lk(mu_);
    return store_.requeue_all();
}

SendOutcome MessageBus::send(const std::string& to, const std::string& from,
                             const std::string& body) {
    const std::string from_key = from.empty() ? "anon" : from;
    std::lock_guard<std::mutex> lk(mu_);

    const long ts = now_secs();
    // Commit to the store BEFORE waking anyone: a woken receiver must find the row.
    const int64_t seq = store_.enqueue(to, from_key, body, ts);
    store_.trim_queued(to, static_cast<int>(inbox_cap_));  // drop oldest (nudges, not bulk)

    auto& sc = counters_[from_key];
    sc.sent++;
    sc.last_seen = ts;

    Inbox& box = inboxes_[to];
    // delivered_live: a receiver is parked OR the target has a fresh presence marker.
    bool live = box.waiters > 0;
    if (!live) {
        auto it = online_.find(to);
        if (it != online_.end() &&
            std::chrono::steady_clock::now() - it->second < kOnlineFresh)
            live = true;
    }
    // Wake every parked receive: one may be a stale session that will exit without
    // claiming, so notify_one could pick it and leave the live receiver asleep.
    box.cv.notify_all();

    SendOutcome out;
    out.seq = seq;
    out.delivered_live = live;
    out.queued_depth = store_.count_queued(to);
    out.your_inbox = store_.count_queued(from_key);
    return out;
}

ReceiveOutcome MessageBus::receive(const std::string& me, long timeout_ms, bool consume,
                                   long cap_ms, const std::string& session) {
    long wait_ms = (timeout_ms <= 0 || timeout_ms > cap_ms) ? cap_ms : timeout_ms;
    std::unique_lock<std::mutex> lk(mu_);
    Inbox& box = inboxes_[me];
    if (!session.empty() && box.live_session != session) {
        box.live_session = session;
        store_.requeue_other_sessions(me, session);
        box.cv.notify_all();  // release parked receives of older sessions
    }
    // A session receive is stale once a newer session for the same name has started.
    auto stale = [&] { return !session.empty() && box.live_session != session; };

    if (store_.count_queued(me) == 0) {
        box.waiters++;
        // Predicate wait: re-checks the store on every wake, so a spurious wake or a
        // message taken by another receiver for the same name re-blocks correctly.
        box.cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                        [&] { return stale() || store_.count_queued(me) > 0; });
        box.waiters--;
    }

    ReceiveOutcome out;
    if (stale()) {
        out.timed_out = true;
        return out;
    }
    if (consume) {
        auto m = store_.claim_next(me, session, now_secs());
        if (!m) {
            out.timed_out = true;
            return out;
        }
        out.messages.push_back(to_bus_msg(std::move(*m)));
        auto& rc = counters_[me];
        rc.received++;
        rc.last_seen = now_secs();
    } else {
        for (auto& m : store_.queued(me)) out.messages.push_back(to_bus_msg(std::move(m)));
        if (out.messages.empty()) out.timed_out = true;
    }
    return out;
}

MessageBus::SweepResult MessageBus::sweep(long now) {
    std::lock_guard<std::mutex> lk(mu_);
    SweepResult r;
    r.requeued = store_.requeue_stale(now - kClaimLease);
    r.pruned = store_.prune(now - kDeliveredKeep);
    if (r.requeued > 0)
        for (auto& [name, box] : inboxes_) box.cv.notify_all();
    return r;
}

std::optional<MessageBus::UnreadHint> MessageBus::unread_hint(const std::string& me) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<StoredMsg> queued = store_.queued(me);
    const int count = static_cast<int>(queued.size());
    const int64_t newest = queued.empty() ? 0 : queued.back().seq;
    auto& last = last_hint_[me];
    if (last == std::make_pair(count, newest)) return std::nullopt;
    last = {count, newest};
    if (count == 0) return std::nullopt;
    UnreadHint h;
    h.count = count;
    for (const auto& m : queued)
        if (std::find(h.from.begin(), h.from.end(), m.from) == h.from.end())
            h.from.push_back(m.from);
    return h;
}

int MessageBus::ack(const std::string& me, const std::vector<int64_t>& seqs) {
    std::lock_guard<std::mutex> lk(mu_);
    return store_.ack(me, seqs, now_secs());
}

ReceiveOutcome MessageBus::peek(const std::string& me) {
    std::lock_guard<std::mutex> lk(mu_);
    ReceiveOutcome out;
    for (auto& m : store_.queued(me)) out.messages.push_back(to_bus_msg(std::move(m)));
    return out;
}

void MessageBus::set_online(const std::string& name, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (on)
        online_[name] = std::chrono::steady_clock::now();
    else
        online_.erase(name);
    counters_[name].last_seen = now_secs();
}

bool MessageBus::is_online(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = online_.find(name);
    return it != online_.end() &&
           std::chrono::steady_clock::now() - it->second < kOnlineFresh;
}

BusStats MessageBus::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = std::chrono::steady_clock::now();

    // Union of every name seen: counters (sent/received), online markers, and names with
    // a queued inbox. A name can appear in only one of these (e.g. a recipient that never
    // sent/received but has queued mail).
    std::unordered_map<std::string, AgentStat> rows;

    for (const auto& [name, c] : counters_) {
        AgentStat& r = rows[name];
        r.name = name;
        r.sent = c.sent;
        r.received = c.received;
        r.last_seen = c.last_seen;
    }
    for (const auto& [name, tp] : online_) {
        AgentStat& r = rows[name];
        r.name = name;
        r.online = (now - tp < kOnlineFresh);
    }
    int queued_total = 0;
    for (const auto& [name, d] : store_.queued_counts()) {
        queued_total += d;
        AgentStat& r = rows[name];
        r.name = name;
        r.inbox_depth = d;
    }

    BusStats s;
    s.queued = queued_total;
    s.total_sent = static_cast<long>(store_.max_seq());  // persistent high-water mark
    for (auto& [name, r] : rows) {
        if (r.online) s.online++;
        s.rows.push_back(std::move(r));
    }
    s.agents = static_cast<int>(s.rows.size());
    std::sort(s.rows.begin(), s.rows.end(),
              [](const AgentStat& a, const AgentStat& b) { return a.name < b.name; });
    return s;
}

}  // namespace mcp
}  // namespace gogol
