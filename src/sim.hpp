// sim.hpp - deterministic network simulator for the Raft core.
//
// The verification strategy of this whole project, borrowed from the
// FoundationDB school: run the cluster inside a SEEDED, single-threaded
// event simulation where the network's every delay, drop, duplicate,
// partition, and crash is a deterministic function of the seed. A failure
// at seed 8571 is not a flaky test, it is a permanent reproduction.
// Invariants are checked after EVERY simulation tick (5 virtual ms), not
// at the end, so a violation is caught within one tick of the state that
// produced it; the ledger and canon oracles have memory, so nothing that
// happens inside a tick can hide behind re-convergence before the check.
//
// The oracles were shaped by mutation testing (see the README): checks with
// no memory let the figure-8 bug survive thousands of random universes,
// because after the rival leader overwrites a committed entry everywhere,
// current states re-converge and pairwise comparison sees agreement. So the
// central oracle here is a LEDGER: the moment any node commits index i, the
// entry at i is frozen forever, and every later state must honor it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "raft.hpp"

namespace sim {

struct Config {
    int n_nodes = 5;
    std::uint64_t min_delay_ms = 1, max_delay_ms = 15;
    double drop_prob = 0.0;
    double duplicate_prob = 0.0;   // P(a sent message arrives twice)
    std::uint64_t election_min_ms = 150, election_max_ms = 300;
    std::uint64_t tick_ms = 5;
};

class Cluster {
public:
    Cluster(std::uint64_t seed, Config cfg = {}) : cfg_(cfg), rng_(seed) {
        for (int i = 0; i < cfg_.n_nodes; ++i) {
            nodes_.emplace_back(i, cfg_.n_nodes);
            nodes_.back().restart(raft::Persistent{}, 0, rand_timeout());
            up_.insert(i);
        }
        applied_len_.assign(static_cast<size_t>(cfg_.n_nodes), 0);
    }

    // ---- chaos controls (all deterministic under the seed) ---------------
    void partition(const std::set<int>& island) { island_ = island; }
    // Block the from->to direction only: the classic one-way link failure.
    void block_oneway(int from, int to) { cut_.insert({from, to}); }
    void heal() { island_.clear(); cut_.clear(); }
    // crash(leader()) with no leader in office passes -1 straight into
    // nodes_[]: undefined behaviour that a mutant can trigger and that
    // then masquerades as a mutant kill (see mutants/README.md, raft-09).
    // Refuse loudly, the way restart() does.
    void crash(int id) {
        if (id < 0 || id >= cfg_.n_nodes) {
            violation = "harness misuse: crash of out-of-range node " +
                        std::to_string(id);
            return;
        }
        up_.erase(id);
        stable_[id] = nodes_[id].stable();
    }
    void restart(int id) {
        // Restarting a node that never crashed would silently wipe its
        // durable state (a disk-loss model nobody asked for), letting it
        // double-vote and manufacture a violation that is a harness
        // artifact, not a core bug. Refuse loudly instead.
        if (up_.count(id) || !stable_.count(id)) {
            violation = "harness misuse: restart of node " +
                        std::to_string(id) + " that was never crashed";
            return;
        }
        up_.insert(id);
        applied_len_[static_cast<size_t>(id)] = 0;   // state machine replays
        if (restart_hook_) restart_hook_(id);
        nodes_[id].restart(stable_[id], now_, rand_timeout());
    }
    void set_drop(double p) { cfg_.drop_prob = p; }

    // Propose through the highest-term live leader: during partitions a
    // deposed stale leader can linger, and the highest term is the real one.
    // Returns false if no live leader accepted the command.
    bool propose(const std::string& cmd) {
        int best = -1;
        for (int id : up_)
            if (nodes_[id].role() == raft::Role::Leader &&
                (best < 0 || nodes_[id].term() > nodes_[best].term()))
                best = id;
        if (best < 0) return false;
        auto idx = nodes_[best].propose(cmd);
        if (!idx) return false;
        // Client contract: if an entry of the acked term ever commits at
        // the acked index, it must be exactly this command. (If a later
        // term commits there instead, the proposal was legitimately
        // superseded -- the ack binds only within its term.)
        pending_[*idx].emplace_back(nodes_[best].term(), cmd);
        return true;
    }

    // Run virtual time forward. Returns false the instant an invariant
    // breaks (details in `violation`).
    bool run_for(std::uint64_t ms) {
        std::uint64_t end = now_ + ms;
        while (now_ < end)
            if (!step()) return false;
        return true;
    }

    // Step until pred(cluster) holds; returns elapsed virtual ms, or
    // nullopt on timeout or invariant violation (check `violation`).
    template <class Pred>
    std::optional<std::uint64_t> run_until(Pred pred, std::uint64_t max_ms) {
        std::uint64_t start = now_, end = now_ + max_ms;
        while (now_ < end) {
            if (!step()) return std::nullopt;
            if (pred(*this)) return now_ - start;
        }
        return std::nullopt;
    }

    // ---- observers -------------------------------------------------------
    int leader() const {
        for (int id : up_)
            if (nodes_[id].role() == raft::Role::Leader) return id;
        return -1;
    }
    const raft::Node& node(int id) const { return nodes_[id]; }
    std::uint64_t now() const { return now_; }
    // The committed-entry ledger: index -> entry, frozen at first commit.
    const std::map<raft::Index, raft::Entry>& ledger() const {
        return committed_;
    }
    // The canonical applied sequence every state machine must reproduce.
    const std::vector<raft::Entry>& canon() const { return canon_; }
    // True when every live node has committed the entire ledger.
    bool converged() const {
        for (int id : up_)
            if (nodes_[id].commit_index() != committed_.size()) return false;
        return true;
    }
    std::string violation;

    // ---- state-machine hooks --------------------------------------------
    // apply hook: called for every entry a node applies, with the node id
    // and the 0-based position in that node's applied stream (restarts
    // replay from 0). Return false to flag a state-machine-level
    // divergence: the run stops with a violation at that exact event.
    // restart hook: called when a node restarts, before its replay begins.
    using ApplyHook =
        std::function<bool(int, const raft::Entry&, std::size_t)>;
    using RestartHook = std::function<void(int)>;
    void set_apply_hook(ApplyHook h) { apply_hook_ = std::move(h); }
    void set_restart_hook(RestartHook h) { restart_hook_ = std::move(h); }

private:
    // ---- portable bounded draws ------------------------------------------
    // std::uniform_int_distribution and std::uniform_real_distribution are
    // implementation-defined: libstdc++, libc++ and MSVC reduce the same
    // mt19937_64 stream differently, so a seed used to reproduce only
    // within one standard library. The two draws below are specified bit
    // for bit, so "seed 8571" names the same universe on every toolchain.
    // (libstdc++ happens to implement the same Lemire reduction, so on the
    // g++ legs the integer draws are bit-identical to the old ones and the
    // [0,1) draw differs only in last-bit rounding; libc++ and MSVC, which
    // reduce differently, now agree with them.)
    //
    // uniform_in(lo, hi) is Lemire's nearly-divisionless method (Lemire,
    // "Fast Random Integer Generation in an Interval", ACM TOMACS 2019):
    // multiply a 64-bit draw by the range width n into a 128-bit product;
    // the high 64 bits are the result, and the draw is rejected when the
    // low 64 bits fall in [0, 2^64 mod n), which makes it exactly uniform.
    // The product's high half is built from 32-bit limbs so no compiler
    // extension (__int128, _umul128) is needed.
    static std::uint64_t mul_hi64(std::uint64_t a, std::uint64_t b) {
        const std::uint64_t mask = 0xffffffffULL;
        std::uint64_t a_lo = a & mask, a_hi = a >> 32;
        std::uint64_t b_lo = b & mask, b_hi = b >> 32;
        std::uint64_t p0 = a_lo * b_lo;
        std::uint64_t p1 = a_lo * b_hi;
        std::uint64_t p2 = a_hi * b_lo;
        std::uint64_t p3 = a_hi * b_hi;
        std::uint64_t mid = (p0 >> 32) + (p1 & mask) + (p2 & mask);
        return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    }
    // Uniform integer in the closed interval [lo, hi].
    std::uint64_t uniform_in(std::uint64_t lo, std::uint64_t hi) {
        if (hi <= lo) return lo;
        std::uint64_t n = hi - lo + 1;
        if (n == 0) return rng_();                 // the full 64-bit range
        std::uint64_t x = rng_();
        std::uint64_t low = x * n;                 // low 64 bits, wraps
        if (low < n) {
            std::uint64_t t = (0 - n) % n;         // 2^64 mod n
            while (low < t) {
                x = rng_();
                low = x * n;
            }
        }
        return lo + mul_hi64(x, n);
    }
    // Uniform double in [0, 1): the top 53 bits of one draw scaled by
    // 2^-53, exact on every IEEE-754 platform.
    double uniform_01() {
        return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
    }
    std::uint64_t rand_timeout() {
        return uniform_in(cfg_.election_min_ms, cfg_.election_max_ms);
    }
    bool blocked(int from, int to) const {
        if (cut_.count({from, to})) return true;
        if (island_.empty()) return false;
        return island_.count(from) != island_.count(to);
    }

    // One tick of virtual time: timers fire, due messages deliver, state
    // machines drain, and every invariant is re-checked.
    bool step() {
        if (!violation.empty()) return false;   // e.g. harness misuse above
        now_ += cfg_.tick_ms;
        std::vector<raft::Message> out;
        for (int id : up_) {
            // Fresh randomized timeout before every timer decision, so a
            // node that hears nothing (the exact situation elections exist
            // for) still redraws each cycle.
            nodes_[id].set_next_timeout(rand_timeout());
            nodes_[id].tick(now_, out);
        }
        enqueue(out);
        deliver_due();
        return drain_applies() && check_invariants();
    }

    void enqueue(std::vector<raft::Message>& msgs) {
        for (auto& m : msgs) {
            if (!up_.count(m.to) || blocked(m.from, m.to)) continue;
            if (uniform_01() < cfg_.drop_prob) continue;
            std::uint64_t delay = uniform_in(cfg_.min_delay_ms,
                                             cfg_.max_delay_ms);
            wire_.emplace(now_ + delay, seq_++, m);
            // The wire may duplicate: the copy takes an independent delay,
            // so a stale short retransmission can arrive AFTER a newer,
            // longer AppendEntries -- the interleaving that makes
            // truncate-on-first-conflict-only load-bearing.
            if (cfg_.duplicate_prob > 0 &&
                uniform_01() < cfg_.duplicate_prob) {
                std::uint64_t dup_delay = uniform_in(cfg_.min_delay_ms,
                                                     cfg_.max_delay_ms);
                wire_.emplace(now_ + dup_delay, seq_++, m);
            }
        }
    }

    void deliver_due() {
        std::vector<raft::Message> out;
        while (!wire_.empty() && std::get<0>(wire_.top()) <= now_) {
            auto [t, s, m] = wire_.top();
            wire_.pop();
            if (!up_.count(m.to) || blocked(m.from, m.to)) continue;
            nodes_[m.to].receive(m, now_, out);
        }
        // Replies travel one network hop per scheduled delivery; only a
        // min_delay of 0 produces same-instant cascades, which settle here.
        if (!out.empty()) {
            enqueue(out);
            deliver_due();
        }
    }

    // Drain every live node's applicable entries and hold them against the
    // canonical applied sequence. This is State Machine Safety made
    // continuous: all nodes apply the identical commands in the identical
    // order, exactly once per epoch, and a restarted node's REPLAY of its
    // committed prefix must reproduce the canon too.
    bool drain_applies() {
        for (int id : up_) {
            for (auto& e : nodes_[id].take_applicable()) {
                size_t k = applied_len_[static_cast<size_t>(id)]++;
                if (k < canon_.size()) {
                    if (e.term != canon_[k].term || e.cmd != canon_[k].cmd) {
                        violation = "applied divergence at position " +
                                    std::to_string(k + 1) + " on node " +
                                    std::to_string(id);
                        return false;
                    }
                } else {
                    canon_.push_back(e);
                }
                // Client contract for this index, if a proposal was acked.
                auto it = pending_.find(static_cast<raft::Index>(k + 1));
                if (it != pending_.end())
                    for (auto& [term, cmd] : it->second)
                        if (term == canon_[k].term && cmd != canon_[k].cmd) {
                            violation = "acked proposal replaced at index " +
                                        std::to_string(k + 1);
                            return false;
                        }
                if (apply_hook_ && !apply_hook_(id, e, k)) {
                    violation = "state machine diverged applying position " +
                                std::to_string(k + 1) + " on node " +
                                std::to_string(id);
                    return false;
                }
            }
        }
        return true;
    }

    bool check_invariants() {
        // 1. Election Safety: at most one leader per term, EVER.
        for (int id : up_) {
            if (nodes_[id].role() != raft::Role::Leader) continue;
            auto t = nodes_[id].term();
            auto it = leaders_by_term_.find(t);
            if (it != leaders_by_term_.end() && it->second != id) {
                violation = "two leaders in term " + std::to_string(t);
                return false;
            }
            leaders_by_term_[t] = id;
        }
        // 2. Commit never exceeds the log: a truncate-below-commit bug must
        // surface as a named violation, not as out-of-bounds reads below.
        for (int id = 0; id < cfg_.n_nodes; ++id) {
            if (nodes_[id].commit_index() > nodes_[id].log().size()) {
                violation = "commit beyond log on node " + std::to_string(id);
                return false;
            }
        }
        // 3. The ledger: once ANY node commits index i, the entry at i is
        // frozen forever. Unlike a pairwise comparison of current states,
        // this has memory -- a figure-8 overwrite is caught even after
        // every node re-converges on the overwritten value. Crashed nodes
        // are checked too: their frozen prefixes were valid when frozen.
        for (int id = 0; id < cfg_.n_nodes; ++id) {
            const auto& lg = nodes_[id].log();
            for (raft::Index i = 1; i <= nodes_[id].commit_index(); ++i) {
                const auto& e = lg[static_cast<size_t>(i - 1)];
                auto it = committed_.find(i);
                if (it == committed_.end()) {
                    committed_[i] = e;
                } else if (it->second.term != e.term ||
                           it->second.cmd != e.cmd) {
                    violation = "committed entry overwritten at index " +
                                std::to_string(i) + " on node " +
                                std::to_string(id);
                    return false;
                }
            }
        }
        // 4. The applied sequence and the ledger agree wherever both exist.
        size_t both = std::min(canon_.size(), committed_.size());
        for (size_t i = 0; i < both; ++i) {
            const auto& c = committed_.at(static_cast<raft::Index>(i + 1));
            if (canon_[i].term != c.term || canon_[i].cmd != c.cmd) {
                violation = "applied sequence diverges from ledger at " +
                            std::to_string(i + 1);
                return false;
            }
        }
        return true;
    }

    Config cfg_;
    std::mt19937_64 rng_;
    std::vector<raft::Node> nodes_;
    std::set<int> up_, island_;
    std::set<std::pair<int, int>> cut_;
    std::map<int, raft::Persistent> stable_;
    std::map<raft::Term, int> leaders_by_term_;
    std::map<raft::Index, raft::Entry> committed_;
    std::vector<raft::Entry> canon_;
    std::vector<size_t> applied_len_;
    std::map<raft::Index,
             std::vector<std::pair<raft::Term, std::string>>> pending_;
    ApplyHook apply_hook_;
    RestartHook restart_hook_;
    std::uint64_t now_ = 0, seq_ = 0;
    using Item = std::tuple<std::uint64_t, std::uint64_t, raft::Message>;
    struct Later {
        bool operator()(const Item& a, const Item& b) const {
            return std::get<0>(a) != std::get<0>(b)
                       ? std::get<0>(a) > std::get<0>(b)
                       : std::get<1>(a) > std::get<1>(b);
        }
    };
    std::priority_queue<Item, std::vector<Item>, Later> wire_;
};

}  // namespace sim
