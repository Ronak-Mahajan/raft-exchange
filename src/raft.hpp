// raft.hpp - Raft consensus core (Ongaro & Ousterhout 2014), logic only.
//
// The core is a PURE state machine: no threads, no sockets, no clocks. It
// consumes (virtual time, messages) and emits messages, which is what makes
// it drivable by the deterministic simulator in sim.hpp - the whole
// verification strategy of this project. Every rule below cites the figure-2
// clause of the paper it implements; the classic subtle bugs (commit-index
// advancement restricted to current-term entries, vote reset on term bump,
// election timer reset conditions) are called out where they live.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raft {

using NodeId = int;
using Term = std::uint64_t;
using Index = std::uint64_t;   // log indices are 1-based; 0 = "before start"

struct Entry {
    Term term = 0;
    std::string cmd;           // opaque command for the state machine
};

// ---- RPCs (figure 2) ------------------------------------------------------
struct RequestVote {
    Term term = 0;
    NodeId candidate = -1;
    Index last_log_index = 0;
    Term last_log_term = 0;
};
struct RequestVoteReply {
    Term term = 0;
    bool granted = false;
};
struct AppendEntries {
    Term term = 0;
    NodeId leader = -1;
    Index prev_log_index = 0;
    Term prev_log_term = 0;
    std::vector<Entry> entries;
    Index leader_commit = 0;
};
struct AppendEntriesReply {
    Term term = 0;
    bool success = false;
    Index match_hint = 0;      // on success: last index now known replicated
};

struct Message {
    NodeId from = -1, to = -1;
    // exactly one engaged
    std::optional<RequestVote> rv;
    std::optional<RequestVoteReply> rvr;
    std::optional<AppendEntries> ae;
    std::optional<AppendEntriesReply> aer;
};

enum class Role { Follower, Candidate, Leader };

// Durable state: what MUST survive a crash (figure 2, "persistent state").
// The simulator's crash/restart keeps exactly this and wipes the rest.
struct Persistent {
    Term current_term = 0;
    std::optional<NodeId> voted_for;
    std::vector<Entry> log;    // log[0] is index 1
};

class Node {
public:
    Node(NodeId id, int n_nodes) : id_(id), n_(n_nodes) {}

    // ---- inputs ----------------------------------------------------------
    // Timer tick at virtual time `now_ms`. May start an election.
    void tick(std::uint64_t now_ms, std::vector<Message>& out);
    // Deliver one message; replies (if any) are appended to `out`.
    void receive(const Message& m, std::uint64_t now_ms,
                 std::vector<Message>& out);
    // Client proposes a command. Only a leader accepts; returns its index.
    std::optional<Index> propose(const std::string& cmd);

    // ---- crash/restart (simulator only) ----------------------------------
    // The simulator snapshots this at crash time, which (single-threaded,
    // crashes only between events) is equivalent to persist-before-send. A
    // real deployment must make p_ durable BEFORE releasing any message
    // that reflects it: before a granted vote leaves, before an
    // AppendEntries success reply leaves, and before the leader counts its
    // own entry toward commitment. Skipping that reintroduces double votes
    // and lost acknowledged entries under real crashes.
    Persistent stable() const { return p_; }
    void restart(const Persistent& p, std::uint64_t now_ms,
                 std::uint64_t election_timeout_ms) {
        p_ = p;
        role_ = Role::Follower;
        commit_ = last_applied_ = 0;
        leader_hint_ = -1;
        votes_.assign(n_, false);
        next_.assign(n_, 1);
        match_.assign(n_, 0);
        deadline_ = now_ms + election_timeout_ms;
        timeout_ms_ = election_timeout_ms;
    }

    // Randomized election timeout used at the NEXT timer arming. The
    // simulator redraws this before every tick from its seeded RNG, so even
    // a fully isolated node re-electing forever gets a fresh draw per cycle
    // -- a fixed per-node period would defeat section 5.2's randomized
    // split-vote avoidance exactly when it matters (no incoming traffic).
    void set_next_timeout(std::uint64_t ms) { timeout_ms_ = ms; }

    // ---- observers -------------------------------------------------------
    Role role() const { return role_; }
    Term term() const { return p_.current_term; }
    Index commit_index() const { return commit_; }
    const std::vector<Entry>& log() const { return p_.log; }
    NodeId id() const { return id_; }
    // Entries the state machine may apply (prefix up to commit_).
    std::vector<Entry> take_applicable() {
        std::vector<Entry> out;
        while (last_applied_ < commit_) {
            out.push_back(p_.log[static_cast<size_t>(last_applied_)]);
            ++last_applied_;
        }
        return out;
    }

private:
    Index last_index() const { return p_.log.size(); }
    Term last_term() const {
        return p_.log.empty() ? 0 : p_.log.back().term;
    }
    Term term_at(Index i) const {           // i is 1-based, 0 -> term 0
        return i == 0 ? 0 : p_.log[static_cast<size_t>(i - 1)].term;
    }

    void become_follower(Term t, std::uint64_t now_ms) {
        // Seeing a higher term ALWAYS resets the vote (figure 2, "all
        // servers"): forgetting this lets a node double-vote across a term
        // bump, which breaks Election Safety.
        if (t > p_.current_term) {
            p_.current_term = t;
            p_.voted_for.reset();
        }
        // A deposed leader or candidate waits a full randomized timeout
        // before campaigning again. Its old deadline is long expired, and
        // reusing it would fire an immediate election against the leader
        // that just deposed it: term inflation and availability churn.
        if (role_ != Role::Follower) deadline_ = now_ms + timeout_ms_;
        role_ = Role::Follower;
    }

    void start_election(std::uint64_t now_ms, std::vector<Message>& out) {
        role_ = Role::Candidate;
        ++p_.current_term;
        p_.voted_for = id_;
        votes_.assign(n_, false);
        votes_[id_] = true;
        deadline_ = now_ms + timeout_ms_;
        for (NodeId peer = 0; peer < n_; ++peer) {
            if (peer == id_) continue;
            Message m;
            m.from = id_; m.to = peer;
            m.rv = RequestVote{p_.current_term, id_, last_index(),
                               last_term()};
            out.push_back(m);
        }
        maybe_win(now_ms, out);            // n = 1 degenerate case
    }

    void become_leader(std::uint64_t now_ms, std::vector<Message>& out) {
        role_ = Role::Leader;
        next_.assign(n_, last_index() + 1);
        match_.assign(n_, 0);
        match_[id_] = last_index();
        // Deliberately NO term-start no-op entry (a dissertation
        // recommendation, not a figure-2 rule): inherited uncommitted
        // entries wait for the next client proposal, since 5.4.2 forbids
        // committing them by count until a current-term entry commits.
        // (That is also why the n = 1 degenerate commit lives in propose(),
        // not here: at election time every entry is from an older term.)
        heartbeat_due_ = now_ms;           // send immediately
        send_heartbeats(now_ms, out);
    }

    void maybe_win(std::uint64_t now_ms, std::vector<Message>& out) {
        int cnt = 0;
        for (bool v : votes_) cnt += v;
        if (role_ == Role::Candidate && cnt * 2 > n_)
            become_leader(now_ms, out);
    }

    void send_heartbeats(std::uint64_t now_ms, std::vector<Message>& out) {
        heartbeat_due_ = now_ms + kHeartbeatMs;
        for (NodeId peer = 0; peer < n_; ++peer) {
            if (peer == id_) continue;
            Index prev = next_[peer] - 1;
            Message m;
            m.from = id_; m.to = peer;
            AppendEntries ae;
            ae.term = p_.current_term;
            ae.leader = id_;
            ae.prev_log_index = prev;
            ae.prev_log_term = term_at(prev);
            for (Index i = next_[peer]; i <= last_index(); ++i)
                ae.entries.push_back(p_.log[static_cast<size_t>(i - 1)]);
            ae.leader_commit = commit_;
            m.ae = std::move(ae);
            out.push_back(m);
        }
    }

    void advance_commit() {
        // Figure 2 leader rule with the section 5.4.2 restriction: a leader
        // may only advance commit_ over entries OF ITS OWN TERM by counting
        // replicas. Committing an older-term entry by count alone is the
        // canonical Raft bug (figure 8): a later leader without that entry
        // can erase it after it was already "committed".
        for (Index n = last_index(); n > commit_; --n) {
            if (term_at(n) != p_.current_term) break;
            int cnt = 0;
            for (NodeId peer = 0; peer < n_; ++peer)
                cnt += (match_[peer] >= n);
            if (cnt * 2 > n_) { commit_ = n; break; }
        }
    }

public:
    static constexpr std::uint64_t kHeartbeatMs = 25;

private:
    NodeId id_;
    int n_;
    Persistent p_;
    Role role_ = Role::Follower;
    Index commit_ = 0, last_applied_ = 0;
    NodeId leader_hint_ = -1;
    std::vector<bool> votes_;
    std::vector<Index> next_, match_;
    std::uint64_t deadline_ = 0;           // election deadline (virtual ms)
    std::uint64_t timeout_ms_ = 150;
    std::uint64_t heartbeat_due_ = 0;
};

// ---- implementation -------------------------------------------------------

inline void Node::tick(std::uint64_t now_ms, std::vector<Message>& out) {
    if (role_ == Role::Leader) {
        if (now_ms >= heartbeat_due_) send_heartbeats(now_ms, out);
        return;
    }
    if (now_ms >= deadline_) start_election(now_ms, out);
}

inline std::optional<Index> Node::propose(const std::string& cmd) {
    if (role_ != Role::Leader) return std::nullopt;
    p_.log.push_back(Entry{p_.current_term, cmd});
    match_[id_] = last_index();
    advance_commit();                      // no-op unless n = 1
    return last_index();
}

inline void Node::receive(const Message& m, std::uint64_t now_ms,
                          std::vector<Message>& out) {
    if (m.rv) {
        const auto& r = *m.rv;
        if (r.term > p_.current_term) become_follower(r.term, now_ms);
        Message reply;
        reply.from = id_; reply.to = m.from;
        RequestVoteReply rr;
        rr.term = p_.current_term;
        // Grant iff same term, vote free (or repeat), and candidate's log is
        // at least as up-to-date (section 5.4.1: compare last term, then
        // last index).
        bool up_to_date =
            r.last_log_term > last_term() ||
            (r.last_log_term == last_term() &&
             r.last_log_index >= last_index());
        if (r.term == p_.current_term && up_to_date &&
            (!p_.voted_for || *p_.voted_for == r.candidate)) {
            p_.voted_for = r.candidate;
            rr.granted = true;
            deadline_ = now_ms + timeout_ms_;   // granting a vote resets timer
        }
        reply.rvr = rr;
        out.push_back(reply);
        return;
    }
    if (m.rvr) {
        const auto& r = *m.rvr;
        if (r.term > p_.current_term) {
            become_follower(r.term, now_ms);
            return;
        }
        if (role_ == Role::Candidate && r.term == p_.current_term &&
            r.granted) {
            votes_[m.from] = true;
            maybe_win(now_ms, out);
        }
        return;
    }
    if (m.ae) {
        const auto& r = *m.ae;
        Message reply;
        reply.from = id_; reply.to = m.from;
        AppendEntriesReply ar;
        if (r.term > p_.current_term) become_follower(r.term, now_ms);
        ar.term = p_.current_term;
        if (r.term < p_.current_term) {
            ar.success = false;
        } else {
            // Valid leader for this term: candidates step down, followers
            // reset their election timer.
            if (role_ == Role::Candidate) role_ = Role::Follower;
            leader_hint_ = r.leader;
            deadline_ = now_ms + timeout_ms_;
            if (r.prev_log_index > last_index() ||
                term_at(r.prev_log_index) != r.prev_log_term) {
                ar.success = false;        // consistency check failed
            } else {
                // Append, truncating on the FIRST conflict only (figure 2
                // step 3): blind truncation at prev_log_index would discard
                // entries a stale-but-valid retransmission never carried.
                Index idx = r.prev_log_index;
                for (const auto& e : r.entries) {
                    ++idx;
                    if (idx <= last_index() &&
                        term_at(idx) != e.term) {
                        p_.log.resize(static_cast<size_t>(idx - 1));
                    }
                    if (idx > last_index()) p_.log.push_back(e);
                }
                if (r.leader_commit > commit_) {
                    Index cap = r.prev_log_index + r.entries.size();
                    commit_ = std::min(r.leader_commit,
                                       std::max(cap, commit_));
                    if (commit_ > last_index()) commit_ = last_index();
                }
                ar.success = true;
                ar.match_hint = r.prev_log_index + r.entries.size();
            }
        }
        reply.aer = ar;
        out.push_back(reply);
        return;
    }
    if (m.aer) {
        const auto& r = *m.aer;
        if (r.term > p_.current_term) {
            become_follower(r.term, now_ms);
            return;
        }
        if (role_ != Role::Leader || r.term != p_.current_term) return;
        if (r.success) {
            if (r.match_hint > match_[m.from]) {
                match_[m.from] = r.match_hint;
                next_[m.from] = r.match_hint + 1;
            }
            advance_commit();
        } else {
            // Back off and retry on next heartbeat -- but never below the
            // confirmed match: a delayed duplicate of an old failure reply
            // must not pin next_ under match_+1 and cause the leader to
            // re-ship the same suffix on every heartbeat forever.
            if (next_[m.from] > match_[m.from] + 1) --next_[m.from];
        }
        return;
    }
}

}  // namespace raft
