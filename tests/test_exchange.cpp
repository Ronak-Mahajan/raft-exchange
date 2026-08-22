// test_exchange.cpp - the matching engine as a replicated state machine.
//
// Layer 1 pins matching semantics (price-time priority, partial fills,
// FIFO within a level, cancels, deterministic rejection) and replay
// determinism at the unit level.
//
// Layer 2 runs the REPLICATED exchange through seeded chaos: random order
// flow proposed through Raft while nodes crash, restart, and partition.
// The oracle, checked at every applied entry in every universe: every
// replica's book -- the full observable state, folded into one hash -- is
// byte-identical at every position of the applied stream, restart replays
// included. That is the property that makes a replicated exchange an
// exchange.
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../src/exchange.hpp"
#include "../src/sim.hpp"

static int failures = 0;
static int g_n = 0;

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

// ---------------------------------------------------------------------------
// Layer 1: matching semantics.
// ---------------------------------------------------------------------------

static void unit_price_time_priority() {
    exch::Exchange x;
    CHECK(x.apply("N 1 S 100 5").empty(), "resting ask makes no fill");
    CHECK(x.apply("N 2 S 101 5").empty(), "second ask rests");
    auto fills = x.apply("N 3 B 101 8");     // sweeps 100 first, then 101
    CHECK(fills.size() == 2, "two fills across two levels");
    if (fills.size() == 2) {
        CHECK(fills[0].maker == 1 && fills[0].px == 100 && fills[0].qty == 5,
              "best price filled first, at the maker's price");
        CHECK(fills[1].maker == 2 && fills[1].px == 101 && fills[1].qty == 3,
              "remainder walks to the next level");
    }
    CHECK(x.best_ask() == 101 && x.open_orders() == 1,
          "partially filled maker still resting");
    CHECK(x.best_bid() == 0, "aggressor fully filled, nothing rests");
}

// The one-character sell-side mirror of the buy sweep: mutation testing
// showed every layer-1 sell aggressor crossed via strict inequality, so a
// sell-at-best-bid bug (crossed book!) shipped green until this test.
static void unit_sell_at_bid() {
    exch::Exchange x;
    x.apply("N 1 B 100 5");
    auto f = x.apply("N 2 S 100 3");
    CHECK(f.size() == 1, "sell at best-bid price trades");
    if (f.size() == 1)
        CHECK(f[0].maker == 1 && f[0].px == 100 && f[0].qty == 3,
              "fills against the resting bid at the maker's price");
    CHECK(x.best_ask() == 0 && x.open_orders() == 1,
          "aggressor fully filled, nothing rests on the ask side");
}

static void unit_fifo_within_level() {
    exch::Exchange x;
    x.apply("N 1 S 100 3");
    x.apply("N 2 S 100 3");
    auto fills = x.apply("N 3 B 100 4");
    CHECK(fills.size() == 2, "one full and one partial fill");
    if (fills.size() == 2)
        CHECK(fills[0].maker == 1 && fills[0].qty == 3 &&
                  fills[1].maker == 2 && fills[1].qty == 1,
              "same price fills in arrival order");
}

static void unit_cancel() {
    exch::Exchange x;
    x.apply("N 1 S 100 5");
    x.apply("C 1");
    CHECK(x.open_orders() == 0, "cancel releases the id");
    auto fills = x.apply("N 2 B 100 5");
    CHECK(fills.empty() && x.best_bid() == 100,
          "cancelled order cannot trade; aggressor rests");
    x.apply("N 1 S 200 5");                   // cancelled id is reusable
    CHECK(x.rejected() == 0 && x.best_ask() == 200,
          "cancelled id is reusable, not a ghost collision");
    auto h = x.state_hash();
    x.apply("C 99");                          // unknown cancel: counted
    CHECK(x.state_hash() != h, "even a rejected command moves the hash");
}

// Absolute accounting values: mutation testing showed cross-replica
// equality is vacuous against deterministic bugs -- every replica halves
// the volume identically. Only absolute assertions kill that class.
static void unit_accounting() {
    exch::Exchange x;
    CHECK(x.best_bid() == 0 && x.best_ask() == 0,
          "empty book reports 0 on both sides");
    x.apply("N 1 S 100 5");
    x.apply("N 2 B 100 5");
    CHECK(x.fill_count() == 1, "fills are counted");
    CHECK(x.volume() == 5, "volume equals traded quantity");
    CHECK(x.best_bid() == 0 && x.best_ask() == 0,
          "book empty again after the full cross");
}

// The hash must cover the fill STREAM as well as the resting book: two
// replicas with identical (empty) books, identical counts, and identical
// volume but fills at different prices must disagree.
static void unit_hash_covers_fills() {
    exch::Exchange a, b;
    a.apply("N 1 S 100 2");
    a.apply("N 2 B 100 2");
    b.apply("N 1 S 101 2");
    b.apply("N 2 B 101 2");
    CHECK(a.fill_count() == 1 && b.fill_count() == 1 &&
              a.volume() == b.volume() && a.open_orders() == 0 &&
              b.open_orders() == 0,
          "setup: same book, same counts, different fill price");
    CHECK(a.state_hash() != b.state_hash(),
          "hash separates identical books with different fill streams");
}

static void unit_deterministic_rejection() {
    exch::Exchange x;
    x.apply("N 1 S 100 5");
    auto before_orders = x.open_orders();
    const char* bad[] = {
        "N 1 B 100 5",            // id collides with an open order
        "N 2 B 0 5",              // zero price
        "N 3 B 100 0",            // zero qty
        "garbage",                //
        "N -4 B 100 5",           // signs are not digits
        "N +5 B 100 5",           //
        "N 6 B 100 5extra",       // trailing bytes
        "N 7 B 100 5 ",           // trailing space
        "N 8  B 100 5",           // double space: non-canonical
        "N 9 B 1000000001 5",     // price above the admission bound
        "N 10 B 100 5\n",         // newline is not part of the grammar
        "C 1 junk",               // trailing bytes on cancel
        "",                       // empty command
    };
    for (auto* c : bad) x.apply(c);
    CHECK(x.open_orders() == before_orders && x.fill_count() == 0,
          "rejected commands change no book state");
    CHECK(x.rejected() == sizeof(bad) / sizeof(bad[0]),
          "every rejection is counted");
}

// A fixed script with a compiled-in expected hash: any build (compiler,
// stdlib, platform) that produces different observable state fails HERE,
// as a unit test, instead of diverging in production across replicas.
static void unit_golden_vector() {
    const char* script[] = {
        "N 1 B 100 5", "N 2 S 99 3",   "N 3 S 101 4", "C 1",
        "N 4 B 102 6", "N 5 S 98 2",   "C 3",         "N 6 B 101 1",
        "N -7 B 100 5", "N 8 B 100 5extra", "C 999",  "N 9 S 103 7",
        "N 10 B 1000000000 1", "N 11 B 103 2",
    };
    exch::Exchange x;
    for (auto* c : script) x.apply(c);
    const std::uint64_t kGolden = 0x34aec28f85ecc911ULL;
    if (x.state_hash() != kGolden)
        std::printf("golden vector hash is now 0x%016llx\n",
                    (unsigned long long)x.state_hash());
    CHECK(x.state_hash() == kGolden,
          "golden vector drifted: same script, different observable state");
}

static void unit_replay_determinism() {
    const char* script[] = {"N 1 B 100 5", "N 2 S 99 3",  "N 3 S 101 4",
                            "C 1",         "N 4 B 102 6", "N 5 S 98 2",
                            "C 3",         "N 6 B 101 1"};
    exch::Exchange a, b;
    for (auto* c : script) a.apply(c);
    for (auto* c : script) b.apply(c);
    CHECK(a.state_hash() == b.state_hash(),
          "identical command sequences produce identical state");
    exch::Exchange d;
    for (int i = 7; i >= 0; --i) d.apply(script[i]);
    CHECK(d.state_hash() != a.state_hash(),
          "different orderings produce different state (hash is sharp)");
}

// ---------------------------------------------------------------------------
// Layer 2: the replicated exchange under chaos.
// ---------------------------------------------------------------------------

static sim::Config with_n(int n) {
    sim::Config c;
    c.n_nodes = n;
    return c;
}

// Random order flow through Raft while nodes crash and partitions cut the
// cluster. Book equality at every applied position is enforced by the
// hook; the end state must converge with every replica byte-identical.
static void scenario_replicated_book(std::uint64_t seed, int n) {
    sim::Cluster c(seed, with_n(n));
    std::vector<exch::Exchange> books(static_cast<size_t>(n));
    std::map<std::size_t, std::uint64_t> canon_hash;
    std::string hook_err;
    c.set_apply_hook([&](int id, const raft::Entry& e, std::size_t k) {
        auto& bk = books[static_cast<size_t>(id)];
        bk.apply(e.cmd);
        // A crossed book (bid >= ask both resting) is unreachable under
        // correct matching; checking it in every universe gives the chaos
        // layer semantic teeth of its own.
        if (bk.best_bid() != 0 && bk.best_ask() != 0 &&
            bk.best_bid() >= bk.best_ask()) {
            hook_err = "book crossed after applied position " +
                       std::to_string(k + 1) + " on node " +
                       std::to_string(id);
            return false;
        }
        auto h = bk.state_hash();
        auto [it, inserted] = canon_hash.try_emplace(k + 1, h);
        if (!inserted && it->second != h) {
            hook_err = "book hash diverged at applied position " +
                       std::to_string(k + 1) + " on node " +
                       std::to_string(id);
            return false;
        }
        return true;
    });
    c.set_restart_hook(
        [&](int id) { books[static_cast<size_t>(id)].reset(); });

    std::mt19937_64 rng(seed * 0x9e3779b97f4a7c15ULL + 1);
    std::uint64_t next_id = 1;
    std::vector<std::uint64_t> open;
    // Hostile flow: deterministic rejects must replicate exactly too.
    const char* garbage[] = {
        "garbage",       "N -1 B 100 5",  "N 1 B 100 5extra",
        "C 424242",      "N 5 X 100 5",   "N 6 B 1000000001 5",
        "N 7 B 100 0",
    };
    auto propose_random = [&]() {
        if (rng() % 8 == 0) {                        // ~12% hostile
            c.propose(garbage[rng() % (sizeof(garbage) / sizeof(char*))]);
            return;
        }
        if (!open.empty() && rng() % 5 == 0) {       // 20% cancels
            size_t i = rng() % open.size();
            c.propose("C " + std::to_string(open[i]));
            open.erase(open.begin() + static_cast<long long>(i));
            return;
        }
        std::uint64_t id = next_id++;
        char side = (rng() % 2) ? 'B' : 'S';
        std::int64_t px = 95 + static_cast<std::int64_t>(rng() % 11);
        std::int64_t qty = 1 + static_cast<std::int64_t>(rng() % 9);
        if (c.propose("N " + std::to_string(id) + ' ' + side + ' ' +
                      std::to_string(px) + ' ' + std::to_string(qty)))
            open.push_back(id);
    };

    REQUIRE(c.run_for(2000), seed, c.violation.c_str());
    int crashed = -1;
    for (int round = 0; round < 12; ++round) {
        for (int i = 0; i < 3; ++i) propose_random();
        if (round % 3 == 2) {                        // churn every 3rd round
            if (crashed >= 0) c.restart(crashed);
            crashed = static_cast<int>(rng() % static_cast<unsigned>(n));
            c.crash(crashed);
        }
        if (round == 5) c.partition({(crashed + 1) % n});
        if (round == 7) c.heal();
        REQUIRE(c.run_for(400), seed,
                hook_err.empty() ? c.violation.c_str() : hook_err.c_str());
    }
    if (crashed >= 0) c.restart(crashed);
    c.heal();
    REQUIRE(c.run_for(3000), seed,
            hook_err.empty() ? c.violation.c_str() : hook_err.c_str());
    size_t before = c.ledger().size();
    REQUIRE(c.propose("N " + std::to_string(next_id++) + " B 100 1"), seed,
            "post-chaos leader refused an order");
    auto t = c.run_until(
        [before](const sim::Cluster& cc) {
            return cc.ledger().size() > before && cc.converged();
        },
        3000);
    REQUIRE(t.has_value(), seed,
            c.violation.empty() ? "post-chaos order never committed"
                                : c.violation.c_str());
    // Every replica ends byte-identical: same book, same fills, same
    // volume, same rejections -- one hash to rule on all of it.
    auto h0 = books[0].state_hash();
    for (int id = 1; id < n; ++id)
        REQUIRE(books[static_cast<size_t>(id)].state_hash() == h0, seed,
                "replicas converged in Raft but diverged in the book");
    for (int id = 1; id < n; ++id)
        REQUIRE(books[static_cast<size_t>(id)].volume() ==
                    books[0].volume(),
                seed, "matched volume differs across replicas");
}

int main() {
    int before = failures;
    unit_price_time_priority();
    unit_sell_at_bid();
    unit_fifo_within_level();
    unit_cancel();
    unit_accounting();
    unit_hash_covers_fills();
    unit_deterministic_rejection();
    unit_golden_vector();
    unit_replay_determinism();
    std::printf("%-14s price-time both sides, FIFO, cancel+reuse, "
                "accounting, hash, grammar, golden, replay: %s\n",
                "engine units",
                failures == before ? "pass" : "FAILURES above");

    const int kSeeds = 150;
    int universes = 0;
    before = failures;
    for (int n : {3, 5, 7}) {
        g_n = n;
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            scenario_replicated_book(seed, n);
            ++universes;
        }
    }
    std::printf("%-14s n=3/5/7 x %d seeds, %s\n", "replicated_book", kSeeds,
                failures == before ? "book identical at every applied "
                                     "position on every replica"
                                   : "FAILURES above");

    if (failures == 0)
        std::printf("\nOK: %d chaos universes, the replicated book never "
                    "diverged\n", universes);
    else
        std::printf("\n%d FAILURES\n", failures);
    return failures ? 1 : 0;
}
