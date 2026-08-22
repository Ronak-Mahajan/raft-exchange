// test_raft.cpp - verification of the Raft core, in three layers.
//
// 1. Scripted adversaries: hand-driven message deliveries forcing the exact
//    interleavings random exploration provably missed. Mutation testing
//    found the misses -- the figure-8 commit bug and blind truncation both
//    survived 8,000 random universes before these tests existed.
// 2. Seeded universes: every scenario across three cluster sizes and
//    hundreds of seeds, with safety invariants (election safety, the
//    committed-entry ledger, state-machine equivalence) checked after every
//    event inside every run. A failure prints the seed and cluster size,
//    which reproduce it exactly.
// 3. Liveness bounds: elections and commits must not merely happen, they
//    must happen within a deadline, measured in virtual milliseconds.
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../src/sim.hpp"

static int failures = 0;
static int g_n = 0;                    // cluster size under test, for repro

#define REQUIRE(cond, seed, what)                                            \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL seed=%llu n=%d: %s\n",                         \
                        (unsigned long long)(seed), g_n, what);              \
            ++failures;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK(cond, what)                                          \
    do {                                                           \
        if (!(cond)) { std::printf("FAIL: %s\n", what); ++failures; } \
    } while (0)

using raft::Message;
using MsgVec = std::vector<Message>;

// ---------------------------------------------------------------------------
// Layer 1: scripted adversaries. Nodes are driven directly, so the exact
// message orderings under test are forced, not hoped for.
// ---------------------------------------------------------------------------

// Deliver every message in `in` whose target is in `targets`; replies
// accumulate in `out`. Everything else is dropped (partition/crash).
static void deliver(std::vector<raft::Node>& ns, const MsgVec& in,
                    const std::set<int>& targets, std::uint64_t now,
                    MsgVec& out) {
    for (const auto& m : in)
        if (targets.count(m.to))
            ns[static_cast<size_t>(m.to)].receive(m, now, out);
}

// The paper's figure 8: a leader must never commit an OLDER-term entry by
// replica count alone (section 5.4.2). Ledger oracle: the moment any node's
// commit index covers index i, the entry at i is frozen forever.
static void unit_figure8() {
    const int n = 5;
    std::vector<raft::Node> ns;
    for (int i = 0; i < n; ++i) ns.emplace_back(i, n);
    std::uint64_t now = 0;
    for (int i = 0; i < n; ++i)
        ns[i].restart(raft::Persistent{}, now, i == 0 ? 10 : 1000000);

    std::map<raft::Index, std::pair<raft::Term, std::string>> ledger;
    auto ledger_check = [&](const char* where) {
        for (auto& nd : ns) {
            for (raft::Index i = 1; i <= nd.commit_index(); ++i) {
                auto& e = nd.log()[static_cast<size_t>(i - 1)];
                auto it = ledger.find(i);
                if (it == ledger.end()) {
                    ledger[i] = {e.term, e.cmd};
                } else if (it->second !=
                           std::make_pair(e.term, e.cmd)) {
                    std::printf("FAIL: committed entry overwritten at idx "
                                "%llu (%s)\n",
                                (unsigned long long)i, where);
                    ++failures;
                    return;
                }
            }
        }
    };

    MsgVec out, replies;
    // Step 1: node0 wins term 1 with votes {0,1,2}; appends "A" (idx 1),
    // replicated to node1 ONLY -> uncommitted at 2/5.
    now = 20; ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    ns[0].propose("A");
    now = 50; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].commit_index() == 0, "A must be uncommitted at 2/5");

    // Step 2: node4 wins term 2 with votes {2,3,4}; appends "B" (idx 1)
    // that never leaves node4. (Its term-1 attempt is starved first.)
    ns[4].restart(ns[4].stable(), now, 10);
    now += 10; out.clear(); ns[4].tick(now, out);
    now += 10; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    out.clear();     deliver(ns, replies, {4}, now, out);
    ns[4].propose("B");

    // Step 3: node0 restarts, wins term 3 (its term-2 attempt is rejected
    // by node2, who voted for node4), and re-replicates the TERM-1 entry
    // "A" to node2 -- now on a majority {0,1,2}. Rule 5.4.2: it must NOT
    // commit by count, because "A" is not from the current term.
    ns[0].restart(ns[0].stable(), now, 10);
    now += 10; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    now += 10; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    now += 25; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    now += 25; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    ledger_check("after old-term entry reached a majority");
    now += 25; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1, 2}, now, replies);
    ledger_check("after commit index propagated");

    // Step 4: node0 dies; node4 (last log term 2 > 1) wins term 4 with
    // votes {2,3,4} and overwrites index 1 with "B". If "A" was wrongly
    // committed in step 3, the ledger fires here.
    ns[4].restart(ns[4].stable(), now, 10);
    now += 10; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    out.clear();     deliver(ns, replies, {4}, now, out);
    now += 10; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    out.clear();     deliver(ns, replies, {4}, now, out);
    now += 25; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    out.clear();     deliver(ns, replies, {4}, now, out);
    now += 25; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    out.clear();     deliver(ns, replies, {4}, now, out);
    ledger_check("after rival leader overwrote index 1");
    now += 25; out.clear(); ns[4].tick(now, out);
    replies.clear(); deliver(ns, out, {2, 3}, now, replies);
    ledger_check("after new leader's commit propagated");
}

// Figure 2 step 3: truncate ONLY on a real term conflict. A stale duplicate
// AppendEntries carrying a shorter prefix of the same entries must be a
// no-op, never a truncation below already-appended (even committed) entries.
static void unit_stale_ae_retransmission() {
    raft::Node f(1, 3);
    f.restart(raft::Persistent{}, 0, 1000000);
    MsgVec out;
    Message m; m.from = 0; m.to = 1;
    raft::AppendEntries ae;
    ae.term = 1; ae.leader = 0; ae.prev_log_index = 0; ae.prev_log_term = 0;
    ae.entries = {{1, "a"}, {1, "b"}, {1, "c"}};
    ae.leader_commit = 3;
    m.ae = ae;
    f.receive(m, 10, out);
    CHECK(f.log().size() == 3, "three entries appended");
    CHECK(f.commit_index() == 3, "all three committed");

    Message m2; m2.from = 0; m2.to = 1;
    raft::AppendEntries ae2;
    ae2.term = 1; ae2.leader = 0; ae2.prev_log_index = 0;
    ae2.prev_log_term = 0;
    ae2.entries = {{1, "a"}};
    ae2.leader_commit = 0;
    m2.ae = ae2;
    out.clear();
    f.receive(m2, 20, out);
    CHECK(f.log().size() == 3,
          "stale retransmission must not truncate the log");
    CHECK(f.commit_index() <= f.log().size(),
          "commit index must never exceed log length");
}

// Committed entries must reach the state machine, in order, exactly once
// per epoch -- and a restarted node's replay reproduces the same sequence.
static void unit_apply_and_replay() {
    const int n = 3;
    std::vector<raft::Node> ns;
    for (int i = 0; i < n; ++i) ns.emplace_back(i, n);
    std::uint64_t now = 0;
    for (int i = 0; i < n; ++i)
        ns[i].restart(raft::Persistent{}, now, i == 0 ? 10 : 1000000);
    MsgVec out, replies;
    now = 20; ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    ns[0].propose("x");
    ns[0].propose("y");
    now = 50; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].commit_index() == 2, "leader committed both entries");
    auto applied = ns[0].take_applicable();
    CHECK(applied.size() == 2 && applied[0].cmd == "x" &&
              applied[1].cmd == "y",
          "committed entries handed to the state machine in order");
    CHECK(ns[0].take_applicable().empty(),
          "no double application of the same entries");

    // Replay: restart node0, re-elect it, commit one more entry. The full
    // applied stream must be x, y, z -- the prefix replayed identically.
    auto s = ns[0].stable();
    ns[0].restart(s, now, 10);
    now += 10; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].role() == raft::Role::Leader, "re-elected after restart");
    ns[0].propose("z");
    now += 25; out.clear(); ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].commit_index() == 3,
          "current-term entry commits the inherited prefix transitively");
    auto replay = ns[0].take_applicable();
    CHECK(replay.size() == 3 && replay[0].cmd == "x" &&
              replay[1].cmd == "y" && replay[2].cmd == "z",
          "replay must reproduce the same sequence");
}

// votedFor semantics: one vote per term, reset exactly on term bump.
static void unit_double_vote() {
    raft::Node f(1, 3);
    f.restart(raft::Persistent{}, 0, 1000000);
    MsgVec out;
    auto rv = [&](raft::Term t, int cand) {
        Message m; m.from = cand; m.to = 1;
        m.rv = raft::RequestVote{t, cand, 0, 0};
        out.clear();
        f.receive(m, 10, out);
        return out.at(0).rvr->granted;
    };
    CHECK(rv(5, 0), "first vote in term 5 granted");
    CHECK(!rv(5, 2), "second candidate in the same term denied");
    CHECK(rv(6, 2), "term bump resets the vote; term-6 candidate granted");
    CHECK(!rv(5, 0), "stale-term request denied");
}

// The n = 1 degenerate cluster is its own majority.
static void unit_single_node() {
    raft::Node n0(0, 1);
    n0.restart(raft::Persistent{}, 0, 10);
    MsgVec out;
    n0.tick(10, out);
    CHECK(n0.role() == raft::Role::Leader, "n=1 elects itself");
    CHECK(n0.propose("solo").has_value(), "n=1 accepts a proposal");
    CHECK(n0.commit_index() == 1, "n=1 commits without any messages");
    auto a = n0.take_applicable();
    CHECK(a.size() == 1 && a[0].cmd == "solo", "n=1 applies");
}

// Even-cluster election arithmetic: half the votes is NOT a majority.
static void unit_elect_quorum() {
    raft::Node a(0, 2);
    a.restart(raft::Persistent{}, 0, 10);
    MsgVec out;
    a.tick(10, out);   // starts election: own vote is 1 of 2
    CHECK(a.role() == raft::Role::Candidate,
          "n=2: a candidate's own vote alone must not elect it");

    // 4-node split brain: two candidates, each with exactly half the votes.
    raft::Node c1(0, 4), c2(3, 4);
    c1.restart(raft::Persistent{}, 0, 10);
    c2.restart(raft::Persistent{}, 0, 10);
    out.clear(); c1.tick(10, out);
    out.clear(); c2.tick(10, out);
    Message g1; g1.from = 1; g1.to = 0;
    g1.rvr = raft::RequestVoteReply{1, true};
    Message g2; g2.from = 2; g2.to = 3;
    g2.rvr = raft::RequestVoteReply{1, true};
    out.clear(); c1.receive(g1, 11, out);
    out.clear(); c2.receive(g2, 11, out);
    CHECK(!(c1.role() == raft::Role::Leader &&
            c2.role() == raft::Role::Leader && c1.term() == c2.term()),
          "n=4: two leaders elected in the same term with 2/4 votes each");
}

// Figure 2: granting a vote re-arms the election timer ("or granting vote
// to candidate"). Without it, a voter campaigns against its own candidate.
static void unit_grant_deadline_reset() {
    raft::Node f(1, 3);
    f.restart(raft::Persistent{}, 0, 100);          // deadline at t=100
    MsgVec out;
    Message m; m.from = 0; m.to = 1;
    m.rv = raft::RequestVote{1, 0, 0, 0};
    f.receive(m, 99, out);                          // grant at t=99
    CHECK(!out.empty() && out.at(0).rvr && out.at(0).rvr->granted,
          "vote granted");
    out.clear();
    f.tick(150, out);   // inside the re-armed window (99+100=199)
    CHECK(f.role() == raft::Role::Follower && out.empty(),
          "voter campaigned against the candidate it just voted for");
}

// A candidate that hears an equal-term AppendEntries from the elected
// leader must step down (section 5.2).
static void unit_candidate_stepdown() {
    raft::Node c(0, 3), l(1, 3);
    c.restart(raft::Persistent{}, 0, 100);
    l.restart(raft::Persistent{}, 0, 90);
    MsgVec out;
    out.clear(); l.tick(90, out);     // l candidate, term 1
    out.clear(); c.tick(100, out);    // c candidate, term 1 (split vote)
    Message g; g.from = 2; g.to = 1;
    g.rvr = raft::RequestVoteReply{1, true};
    out.clear(); l.receive(g, 101, out);   // l wins term 1, heartbeats out
    CHECK(l.role() == raft::Role::Leader, "l elected");
    MsgVec o2;
    for (auto& m2 : out)
        if (m2.to == 0 && m2.ae) c.receive(m2, 102, o2);
    CHECK(c.role() == raft::Role::Follower,
          "rival candidate did not step down on equal-term AppendEntries");
}

// Granted replies from a PREVIOUS term's election must not count toward
// the current tally: replies delayed past the election timeout otherwise
// elect a leader with no current-term majority. (Found by a novel-mutant
// hunt: no seeded network config let replies outlive an election, so the
// term guard on the tally was unfalsifiable before this test.)
static void unit_stale_vote_count() {
    raft::Node c(0, 5);
    c.restart(raft::Persistent{}, 0, 100);
    MsgVec out;
    c.tick(100, out);   // election #1: term 1
    CHECK(c.term() == 1, "first election at term 1");
    out.clear();
    c.tick(200, out);   // timeout: election #2, term 2, tally reset to self
    CHECK(c.term() == 2 && c.role() == raft::Role::Candidate,
          "second election at term 2");
    Message m1; m1.from = 1; m1.to = 0;
    m1.rvr = raft::RequestVoteReply{1, true};
    Message m2; m2.from = 2; m2.to = 0;
    m2.rvr = raft::RequestVoteReply{1, true};
    out.clear();
    c.receive(m1, 201, out);
    c.receive(m2, 201, out);
    CHECK(c.role() != raft::Role::Leader,
          "leader elected in term 2 on term-1 votes (no term-2 majority)");
}

// Figure 2 AE receiver rule 5 is min(leaderCommit, index of last new
// entry). This leader always ships the full suffix, so the clamp is dead
// code inside the closed system -- but Node::receive is public API, and a
// leader that pages its entries legally ships leader_commit beyond the
// carried suffix. Defense-in-depth for the day send_heartbeats batches.
static void unit_commit_clamp() {
    raft::Node f(1, 3);
    f.restart(raft::Persistent{}, 0, 1000000);
    Message m; m.from = 0; m.to = 1;
    raft::AppendEntries ae;
    ae.term = 1; ae.leader = 0;
    ae.prev_log_index = 0; ae.prev_log_term = 0;
    ae.entries = {{1, "a"}};        // one entry of a longer committed log
    ae.leader_commit = 7;           // leader's commit is far ahead
    m.ae = ae;
    MsgVec out;
    f.receive(m, 10, out);
    CHECK(f.commit_index() <= f.log().size(),
          "commit index ran past the log on a partial-suffix AppendEntries");
    CHECK(f.commit_index() == 1, "commit clamps to last new entry");
}

// A deposed leader waits a FULL randomized timeout before campaigning
// again; its stale leader-era deadline must not fire an instant
// counter-election (term inflation, availability churn).
static void unit_depose_deadline_reset() {
    raft::Node a(0, 3);
    a.restart(raft::Persistent{}, 0, 100);
    MsgVec out;
    a.tick(100, out);                        // candidate term 1
    Message g; g.from = 1; g.to = 0;
    g.rvr = raft::RequestVoteReply{1, true};
    out.clear(); a.receive(g, 105, out);     // leader term 1
    CHECK(a.role() == raft::Role::Leader, "a elected");
    a.propose("x");                          // a's log now beats empty logs
    // Long after, a laggard rival with an EMPTY log campaigns at term 2:
    // a is deposed by the higher term but must not grant (log check) and
    // must not campaign instantly off its ancient leader-era deadline.
    Message rv; rv.from = 2; rv.to = 0;
    rv.rv = raft::RequestVote{2, 2, 0, 0};
    out.clear(); a.receive(rv, 5000, out);
    CHECK(a.role() == raft::Role::Follower && a.term() == 2, "a deposed");
    CHECK(out.at(0).rvr && !out.at(0).rvr->granted, "empty log not granted");
    out.clear();
    a.tick(5001, out);   // must stay quiet until ~5100
    CHECK(a.role() == raft::Role::Follower && a.term() == 2 && out.empty(),
          "deposed leader campaigned instantly instead of waiting a timeout");
}

// Section 5.4.1's INDEX tiebreak: same last term, shorter log -> no vote.
// (The term half of the comparison is pinned by figure8; this pins the
// other half, which one lossy seed in 450 was the only thing catching.)
static void unit_uptodate_index() {
    raft::Node f(1, 3);
    raft::Persistent p;
    p.current_term = 1;
    p.log = {{1, "a"}, {1, "b"}};
    f.restart(p, 0, 1000000);
    MsgVec out;
    auto rv = [&](raft::Index last_idx) {
        Message m; m.from = 0; m.to = 1;
        m.rv = raft::RequestVote{2, 0, last_idx, 1};
        out.clear();
        f.receive(m, 10, out);
        return out.at(0).rvr->granted;
    };
    CHECK(!rv(1), "same last term, shorter log must be denied");
    CHECK(rv(2), "same last term, equal length must be granted");
}

// Quorum arithmetic on the even cluster: 1 of 2 is NOT a majority.
static void unit_minority_stall() {
    std::vector<raft::Node> ns;
    for (int i = 0; i < 2; ++i) ns.emplace_back(i, 2);
    ns[0].restart(raft::Persistent{}, 0, 10);
    ns[1].restart(raft::Persistent{}, 0, 1000000);
    MsgVec out, replies;
    std::uint64_t now = 10;
    ns[0].tick(now, out);
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].role() == raft::Role::Leader, "two-node cluster elects");
    ns[0].propose("x");
    now += 25; out.clear(); ns[0].tick(now, out);   // peer never answers
    CHECK(ns[0].commit_index() == 0,
          "quorum of a two-node cluster is two: no commit alone");
    replies.clear(); deliver(ns, out, {1}, now, replies);
    out.clear();     deliver(ns, replies, {0}, now, out);
    CHECK(ns[0].commit_index() == 1, "commit after the second ack");
}

// ---------------------------------------------------------------------------
// Layer 2: seeded universes.
// ---------------------------------------------------------------------------

static sim::Config with_n(int n) {
    sim::Config c;
    c.n_nodes = n;
    return c;
}

static int count_leaders(const sim::Cluster& c, int n) {
    int k = 0;
    for (int id = 0; id < n; ++id)
        if (c.node(id).role() == raft::Role::Leader) ++k;
    return k;
}

// A quiet cluster elects one leader, commits and APPLIES client commands in
// exact proposal order on every node.
static void scenario_basic(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    REQUIRE(c.leader() >= 0, seed, "no leader elected in 2s of calm");
    for (int i = 0; i < 20; ++i) {
        c.propose("cmd" + std::to_string(i));
        REQUIRE(c.run_for(50), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(1000), seed, c.violation.c_str());
    REQUIRE(c.leader() >= 0, seed, "leader lost without faults");
    REQUIRE(c.canon().size() == 20, seed, "20 proposals did not all apply");
    for (int i = 0; i < 20; ++i)
        REQUIRE(c.canon()[static_cast<size_t>(i)].cmd ==
                    "cmd" + std::to_string(i),
                seed, "applied sequence out of order");
    REQUIRE(c.converged(), seed, "cluster did not converge in calm");
}

// Partition the leader away; the majority elects a successor and moves on;
// after heal the isolated leader's uncommitted tail must be erased and
// every node must converge on the majority history.
static void scenario_partition(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    int old_leader = c.leader();
    REQUIRE(old_leader >= 0, seed, "no initial leader");
    REQUIRE(count_leaders(c, n) == 1, seed, "stale co-leader before fault");
    for (int i = 0; i < 5; ++i) {
        REQUIRE(c.propose("pre" + std::to_string(i)), seed, "pre refused");
        REQUIRE(c.run_for(50), seed, c.violation.c_str());
    }
    c.partition({old_leader});
    REQUIRE(c.propose("orphan"), seed, "isolated leader refused proposal");
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    bool majority_leader = false;
    for (int id = 0; id < n; ++id)
        if (id != old_leader && c.node(id).role() == raft::Role::Leader)
            majority_leader = true;
    REQUIRE(majority_leader, seed, "majority failed to elect around leader");
    for (int i = 0; i < 5; ++i) {
        REQUIRE(c.propose("post" + std::to_string(i)), seed, "post refused");
        REQUIRE(c.run_for(50), seed, c.violation.c_str());
    }
    c.heal();
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    REQUIRE(c.leader() >= 0, seed, "no leader after heal");
    REQUIRE(c.converged(), seed, "cluster did not converge after heal");
    REQUIRE(c.ledger().size() == 10, seed, "committed history wrong size");
    for (auto& [idx, e] : c.ledger())
        REQUIRE(e.cmd != "orphan", seed, "uncommittable entry committed");
    for (int id = 0; id < n; ++id)
        for (auto& e : c.node(id).log())
            REQUIRE(e.cmd != "orphan", seed,
                    "orphan survived in a log after heal");
}

// Crash-and-restart churn: persistent state survives, volatile state is
// rebuilt, and consistency alone proves little (a cluster that commits
// nothing never diverges), so the churned cluster must also still commit.
static void scenario_crash_restart(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    for (int round = 0; round < 8; ++round) {
        c.propose("r" + std::to_string(round));
        int victim = static_cast<int>(rng() % static_cast<unsigned>(n));
        c.crash(victim);
        REQUIRE(c.run_for(400), seed, c.violation.c_str());
        c.restart(victim);
        REQUIRE(c.run_for(400), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(3000), seed, c.violation.c_str());
    REQUIRE(c.leader() >= 0, seed, "no leader after churn settled");
    size_t before = c.ledger().size();
    REQUIRE(c.propose("post-churn"), seed, "settled leader refused proposal");
    REQUIRE(c.run_for(1000), seed, c.violation.c_str());
    REQUIRE(c.ledger().size() > before, seed,
            "post-churn proposal never committed");
    REQUIRE(c.converged(), seed, "cluster did not converge after churn");
}

// Lossy network: 20% of messages vanish. Slower, never inconsistent, and
// still eventually live.
static void scenario_lossy(std::uint64_t seed, int n) {
    sim::Config cfg = with_n(n);
    cfg.drop_prob = 0.20;
    sim::Cluster c(seed, cfg);
    REQUIRE(c.run_for(8000), seed, c.violation.c_str());
    for (int i = 0; i < 10; ++i) {
        c.propose("lossy" + std::to_string(i));
        REQUIRE(c.run_for(200), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(4000), seed, c.violation.c_str());
    auto t = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 2000);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "no leader on lossy network"
                                : c.violation.c_str());
    size_t before = c.ledger().size();
    REQUIRE(c.propose("final"), seed, "lossy leader refused proposal");
    auto t2 = c.run_until(
        [before](const sim::Cluster& cc) {
            return cc.ledger().size() > before;
        },
        3000);
    REQUIRE(t2.has_value(), seed,
            c.violation.empty() ? "proposal never committed on lossy network"
                                : c.violation.c_str());
}

// Delays longer than the heartbeat interval plus wire duplication: stale
// AppendEntries genuinely arrive after newer ones, so figure 2 step 3's
// truncate-on-first-conflict-only rule is load-bearing here. (Mutation
// testing showed the tamer network could never exercise it.)
static void scenario_reorder_duplicate(std::uint64_t seed, int n) {
    sim::Config cfg = with_n(n);
    cfg.max_delay_ms = 60;
    cfg.duplicate_prob = 0.15;
    cfg.drop_prob = 0.05;
    sim::Cluster c(seed, cfg);
    REQUIRE(c.run_for(4000), seed, c.violation.c_str());
    for (int i = 0; i < 10; ++i) {
        c.propose("dup" + std::to_string(i));
        REQUIRE(c.run_for(100), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(3000), seed, c.violation.c_str());
    auto t = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 2000);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "no leader on reordering network"
                                : c.violation.c_str());
    size_t before = c.ledger().size();
    REQUIRE(c.propose("final"), seed, "leader refused proposal");
    auto t2 = c.run_until(
        [before](const sim::Cluster& cc) {
            return cc.ledger().size() > before;
        },
        3000);
    REQUIRE(t2.has_value(), seed,
            c.violation.empty() ? "proposal never committed under reordering"
                                : c.violation.c_str());
}

// One-way link failure: the leader can send but hears nothing back. Its
// heartbeats keep followers loyal, so no election fires -- and with no
// acks, nothing new may commit. Basic Raft livelocks here BY DESIGN
// (check-quorum is the known extension); what must hold unconditionally is
// safety: no commit without a majority of acks, no spurious depositions.
static void scenario_oneway(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    int ld = c.leader();
    REQUIRE(ld >= 0, seed, "no leader");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(c.propose("pre" + std::to_string(i)), seed, "pre refused");
        REQUIRE(c.run_for(50), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(500), seed, c.violation.c_str());
    REQUIRE(c.ledger().size() == 3, seed, "pre entries did not commit");
    raft::Term term_before = c.node(ld).term();
    for (int id = 0; id < n; ++id)
        if (id != ld) c.block_oneway(id, ld);
    for (int i = 0; i < 3; ++i)
        REQUIRE(c.propose("deaf" + std::to_string(i)), seed, "deaf refused");
    REQUIRE(c.run_for(1500), seed, c.violation.c_str());
    REQUIRE(c.ledger().size() == 3, seed,
            "committed without a majority of acks");
    REQUIRE(c.node(ld).role() == raft::Role::Leader &&
                c.node(ld).term() == term_before,
            seed, "one-way isolation must not depose the sending leader");
    c.heal();
    auto t = c.run_until(
        [](const sim::Cluster& cc) {
            return cc.ledger().size() == 6 && cc.converged();
        },
        2000);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "deaf entries never committed after heal"
                                : c.violation.c_str());
}

// Election timeouts barely above the heartbeat interval: the paper's
// timing assumption (broadcast << election timeout) is broken, so Raft
// legitimately churns and liveness is forfeit. Safety must hold anyway --
// it never depends on timing.
static void scenario_timing_stress(std::uint64_t seed, int n) {
    sim::Config cfg = with_n(n);
    cfg.election_min_ms = 35;
    cfg.election_max_ms = 70;
    sim::Cluster c(seed, cfg);
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    for (int i = 0; i < 10; ++i) {
        c.propose("t" + std::to_string(i));
        REQUIRE(c.run_for(100), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(3000), seed, c.violation.c_str());
}

// Delays up to 400 ms against 150-300 ms election timeouts: replies
// routinely outlive the election that requested them, so every stale-reply
// term guard is load-bearing. (A novel-mutant hunt showed that with tamer
// wires, a candidate counting old-term votes was undetectable: the window
// never opened.) Liveness is legitimately poor here; safety must hold.
static void scenario_slow_wire(std::uint64_t seed, int n) {
    sim::Config cfg = with_n(n);
    cfg.max_delay_ms = 400;
    sim::Cluster c(seed, cfg);
    REQUIRE(c.run_for(4000), seed, c.violation.c_str());
    for (int i = 0; i < 10; ++i) {
        c.propose("s" + std::to_string(i));
        REQUIRE(c.run_for(100), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(4000), seed, c.violation.c_str());
}

// A 4-node cluster split 2-2: NEITHER side has a quorum, so no new leader
// may appear in any newer term and nothing new may commit until heal.
// (Even sizes are where election-quorum off-by-ones live; every n in the
// main matrix is odd, where > and >= majorities coincide.)
static void scenario_even_split(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    int ld = c.leader();
    REQUIRE(ld >= 0, seed, "no initial leader");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(c.propose("pre" + std::to_string(i)), seed, "pre refused");
        REQUIRE(c.run_for(50), seed, c.violation.c_str());
    }
    REQUIRE(c.run_for(500), seed, c.violation.c_str());
    REQUIRE(c.ledger().size() == 3, seed, "pre entries did not commit");
    raft::Term t0 = c.node(ld).term();
    c.partition({ld, (ld + 1) % n});           // leader plus one: 2 vs 2
    c.propose("split");                        // may never commit
    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    REQUIRE(c.ledger().size() == 3, seed,
            "half a cluster committed without a quorum");
    for (int id = 0; id < n; ++id)
        REQUIRE(!(c.node(id).role() == raft::Role::Leader &&
                  c.node(id).term() > t0),
                seed, "half a cluster elected a new leader without quorum");
    c.heal();
    REQUIRE(c.run_for(3000), seed, c.violation.c_str());
    size_t before = c.ledger().size();
    REQUIRE(c.propose("final"), seed, "post-heal leader refused proposal");
    auto t = c.run_until(
        [before](const sim::Cluster& cc) {
            return cc.ledger().size() > before && cc.converged();
        },
        3000);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "no commit after even-split heal"
                                : c.violation.c_str());
}

// ---------------------------------------------------------------------------
// Layer 3: liveness bounds, in virtual milliseconds. Deterministic, so a
// bound violation is a permanent reproduction, not a flake.
// ---------------------------------------------------------------------------

struct Bound { std::uint64_t max = 0; };

static void note(Bound& b, std::uint64_t v) { if (v > b.max) b.max = v; }

static void bounds_cold_start(std::uint64_t seed, Bound& b) {
    sim::Cluster c(seed);
    auto t = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 900);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "cold start: no leader within 900 ms"
                                : c.violation.c_str());
    note(b, *t);
}

static void bounds_failover(std::uint64_t seed, Bound& b) {
    sim::Cluster c(seed);
    auto t0 = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 900);
    REQUIRE(t0.has_value(), seed, "no initial leader");
    REQUIRE(c.propose("x"), seed, "refused");
    REQUIRE(c.run_for(200), seed, c.violation.c_str());
    c.crash(c.leader());
    auto t = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 900);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "failover: no new leader within 900 ms"
                                : c.violation.c_str());
    note(b, *t);
}

static void bounds_commit(std::uint64_t seed, Bound& b) {
    sim::Cluster c(seed);
    auto t0 = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 900);
    REQUIRE(t0.has_value(), seed, "no leader");
    REQUIRE(c.run_for(500), seed, c.violation.c_str());
    size_t before = c.ledger().size();
    REQUIRE(c.propose("only"), seed, "refused");
    auto t = c.run_until(
        [before](const sim::Cluster& cc) {
            return cc.ledger().size() > before && cc.converged();
        },
        300);
    REQUIRE(t.has_value(), seed,
            c.violation.empty()
                ? "commit did not reach every node within 300 ms"
                : c.violation.c_str());
    note(b, *t);
}

static void bounds_heal(std::uint64_t seed, Bound& b) {
    sim::Cluster c(seed);
    auto t0 = c.run_until(
        [](const sim::Cluster& cc) { return cc.leader() >= 0; }, 900);
    REQUIRE(t0.has_value(), seed, "no leader");
    int ld = c.leader();
    REQUIRE(c.propose("pre"), seed, "refused");
    REQUIRE(c.run_for(200), seed, c.violation.c_str());
    c.partition({ld});
    auto t1 = c.run_until(
        [ld](const sim::Cluster& cc) {
            for (int id = 0; id < 5; ++id)
                if (id != ld && cc.node(id).role() == raft::Role::Leader)
                    return true;
            return false;
        },
        900);
    REQUIRE(t1.has_value(), seed,
            c.violation.empty() ? "majority did not elect around leader"
                                : c.violation.c_str());
    REQUIRE(c.propose("mid"), seed, "refused");
    REQUIRE(c.run_for(200), seed, c.violation.c_str());
    c.heal();
    auto t = c.run_until(
        [](const sim::Cluster& cc) { return cc.converged(); }, 900);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "heal: no reconvergence within 900 ms"
                                : c.violation.c_str());
    note(b, *t);
}

// ---------------------------------------------------------------------------

int main() {
    int before = failures;
    unit_figure8();
    unit_stale_ae_retransmission();
    unit_apply_and_replay();
    unit_double_vote();
    unit_single_node();
    unit_elect_quorum();
    unit_grant_deadline_reset();
    unit_candidate_stepdown();
    unit_stale_vote_count();
    unit_commit_clamp();
    unit_depose_deadline_reset();
    unit_uptodate_index();
    unit_minority_stall();
    std::printf("%-14s figure8, stale-AE, apply+replay, votes x3, "
                "quorums x3, timers x2, clamp: %s\n", "scripted",
                failures == before ? "pass" : "FAILURES above");

    const int kSeeds = 150;
    const int sizes[] = {3, 5, 7};
    struct { const char* name; void (*fn)(std::uint64_t, int); }
    scenarios[] = {
        {"basic", scenario_basic},
        {"partition", scenario_partition},
        {"crash_restart", scenario_crash_restart},
        {"lossy", scenario_lossy},
        {"reorder_dup", scenario_reorder_duplicate},
        {"oneway", scenario_oneway},
        {"slow_wire", scenario_slow_wire},
    };
    int universes = 0;
    for (auto& s : scenarios) {
        before = failures;
        for (int n : sizes) {
            g_n = n;
            for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
                s.fn(seed, n);
                ++universes;
            }
        }
        std::printf("%-14s n=3/5/7 x %d seeds, %s\n", s.name, kSeeds,
                    failures == before ? "all invariants held"
                                       : "FAILURES above");
    }
    before = failures;
    g_n = 4;
    for (std::uint64_t seed = 1; seed <= 150; ++seed) {
        scenario_even_split(seed, 4);
        ++universes;
    }
    std::printf("%-14s n=4 x 150 seeds, %s\n", "even_split",
                failures == before ? "all invariants held"
                                   : "FAILURES above");
    before = failures;
    g_n = 5;
    for (std::uint64_t seed = 1; seed <= 250; ++seed) {
        scenario_timing_stress(seed, 5);
        ++universes;
    }
    std::printf("%-14s 250 seeds, %s (safety only: liveness is forfeit "
                "when broadcast time ~ election timeout)\n",
                "timing_stress",
                failures == before ? "all invariants held"
                                   : "FAILURES above");

    Bound cold, fo, cm, hl;
    g_n = 5;
    for (std::uint64_t seed = 1; seed <= 250; ++seed) {
        bounds_cold_start(seed, cold);
        bounds_failover(seed, fo);
        bounds_commit(seed, cm);
        bounds_heal(seed, hl);
        universes += 4;
    }
    std::printf("%-14s 250 seeds, virtual ms: cold-start max %llu <= 900, "
                "failover max %llu <= 900,\n%-14s commit-on-all max %llu "
                "<= 300, heal-reconverge max %llu <= 900\n",
                "liveness", (unsigned long long)cold.max,
                (unsigned long long)fo.max, "",
                (unsigned long long)cm.max, (unsigned long long)hl.max);

    if (failures == 0)
        std::printf("\nOK: %d seeded universes + scripted adversaries, "
                    "invariants checked after every event\n", universes);
    else
        std::printf("\n%d FAILURES\n", failures);
    return failures ? 1 : 0;
}
