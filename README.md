# raft-exchange

[![ci](https://github.com/Ronak-Mahajan/raft-exchange/actions/workflows/ci.yml/badge.svg)](https://github.com/Ronak-Mahajan/raft-exchange/actions/workflows/ci.yml)

A distributed exchange built the hard way: a Raft consensus core written from
the paper, verified by deterministic simulation and mutation testing before
any networking exists, with a matching engine as the replicated state
machine.

The premise: an exchange is the worst-case consensus workload. Orders are not
idempotent, ordering is the product, and a split-brain that double-fills one
order is not a bug you apologize for. So the consensus layer gets built and
attacked first, alone, until it survives everything a seeded adversary can
generate. Only then does it earn a matching engine on top.

## What exists today (Phases A and B)

- **`src/raft.hpp`** is the Raft core (Ongaro & Ousterhout 2014) as a pure
  state machine: no threads, no sockets, no clocks. It consumes virtual time
  and messages, and emits messages. Every rule cites the figure-2 clause it
  implements, and the classic subtle bugs are guarded where they live:
  - commit-index advancement restricted to current-term entries (the
    section 5.4.2 rule; skipping it is the figure-8 bug, where a "committed"
    entry from an older term gets erased by a later leader)
  - vote reset exactly on term bump (forgetting it allows double voting)
  - truncate-on-first-conflict only (blind truncation at `prevLogIndex` lets
    a stale retransmission erase newer valid entries)
  - the section 5.4.1 up-to-date check before granting votes
  - persistence boundary matching figure 2 exactly: `currentTerm`,
    `votedFor`, `log[]` survive a crash, nothing else does
- **`src/sim.hpp`** is a deterministic network simulator in the FoundationDB
  style. Every delay, drop, duplicate, partition, and crash is a function of
  one seed; a failure at seed 8571 is a permanent reproduction, not a flake.
- **`src/exchange.hpp`** is the replicated state machine: a deterministic
  price-time-priority matching engine. Integer ticks only, ordered
  containers only, no clocks, no randomness; every input, malformed ones
  included, has exactly one defined outcome. The entire observable state
  (resting book with FIFO queue order, every fill ever produced, every
  rejection) folds into one FNV-1a hash, so two replicas agree if and
  only if one number agrees.
- **`tests/test_raft.cpp`** runs three layers: scripted adversaries forcing
  the interleavings random search misses, seeded scenario sweeps at three
  cluster sizes, and measured liveness bounds.
- **`tests/test_exchange.cpp`** covers matching semantics at the unit level,
  then the replicated exchange under chaos: random order flow proposed
  through Raft while nodes crash, restart, and partition, with the book
  hash checked at **every applied position on every replica**, restart
  replays included.

```
scripted       figure8, stale-AE, stale-AE-reply, apply+replay, votes x3, quorums x3,
               timers x2, clamp: pass
basic          n=3/5/7 x 150 seeds, all invariants held
partition      n=3/5/7 x 150 seeds, all invariants held
crash_restart  n=3/5/7 x 150 seeds, all invariants held
lossy          n=3/5/7 x 150 seeds, all invariants held
reorder_dup    n=3/5/7 x 150 seeds, all invariants held
oneway         n=3/5/7 x 150 seeds, all invariants held
slow_wire      n=3/5/7 x 150 seeds, all invariants held
even_split     n=4 x 150 seeds, all invariants held
timing_stress  250 seeds, all invariants held (safety only)
liveness       250 seeds, virtual ms: cold-start max 465 <= 900, failover
               max 570 <= 900, commit-on-all max 90 <= 300,
               heal-reconverge max 40 <= 900

OK: 4550 seeded universes + scripted adversaries, invariants checked after
every tick
```

(The bounds are what the suite asserts; the maxima are what the current
build prints, and every CI log reprints them. Because the simulator's draws
are portable, see "Seeds are portable" below, a leg that prints different
maxima is a bug, not a platform difference.)

```
engine units    price-time, FIFO, cancel, rejection, replay: pass
replicated_book n=3/5/7 x 150 seeds, book identical at every applied
                position on every replica

OK: 450 chaos universes, the replicated book never diverged
```

## The oracles were earned, not assumed

The first version of this suite ran 4 scenarios across 1,000 seeded
universes and passed. Then it was mutation-tested: re-introduce a known bug,
and a suite worth trusting must fail. Two of the five mutants **survived
8,000 universes**:

- **Deleting the figure-8 guard** (commit older-term entries by count, the
  canonical Raft safety bug) changed nothing. Two blind spots compounded:
  random crash churn never produces the figure-8 interleaving, and the
  oracle compared *current* logs pairwise, so once the rival leader
  overwrote the "committed" entry on every node, the states re-converged
  and looked consistent. The violation had no witness.
- **Blind log truncation** also survived, for a structural reason: the
  simulated network's max delay (15 ms) was shorter than the heartbeat
  interval (25 ms), so consecutive AppendEntries could never cross in
  flight, and the wire never duplicated. The stale-retransmission
  interleaving that truncation protects against was unrepresentable.

A second round hunted NOVEL mutants after the first fixes landed, and found
three more survivors. The sharpest was an Election Safety hole the network
model structurally could not reach: dropping the term guard on vote
counting (a candidate tallying granted replies from its *previous*
election) changed nothing across 3,950 universes, because every simulated
round trip was shorter than the minimum election timeout, so a stale reply
never existed. The fix is a wire whose delays exceed the election timeout
(`slow_wire`), plus a scripted test that delivers yesterday's votes to
today's candidate. The same round caught that every seeded cluster size was
odd, where `>` and `>=` majorities coincide, which hid an election-quorum
off-by-one; `even_split` (a 4-node cluster split 2-2, where neither side
may elect or commit) closes that. All twelve mutants across both rounds are
now killed by the suite.

Every oracle in the current suite exists because a mutant demanded it:

1. **The committed-entry ledger.** The moment *any* node commits index i,
   the entry at i is frozen forever, and every node's state is held against
   it after every tick. Memory is what catches figure 8: re-convergence on
   overwritten history no longer hides the crime.
2. **The applied-sequence canon.** Every node's state machine drains
   through the simulator, which enforces that all nodes apply the identical
   commands in the identical order, exactly once per epoch, and that a
   restarted node's replay reproduces the canon. This is the property the
   matching engine will actually stand on (State Machine Safety), checked
   continuously in all 4,550 universes.
3. **A wire that fights back.** Configurable duplication and delays longer
   than the heartbeat interval, so stale AppendEntries genuinely arrive
   after newer ones and first-conflict truncation is load-bearing.
4. **Scripted adversaries.** The figure-8 interleaving and the stale
   retransmission, driven message by message, because some interleavings
   deserve a guaranteed appearance rather than a probabilistic one.

The same review fixed three real (non-mutant) findings: an isolated node
re-elected forever with a frozen timeout, because the simulator only redrew
randomized timeouts on message receipt and the moment elections matter is
precisely when messages have stopped (now redrawn before every tick); a
deposed leader kept
its long-expired election deadline and immediately fired a disruptive
election (now waits a full randomized timeout); and single-node clusters
could never commit because commit advancement only ran in the reply handler
(now also runs at propose time).

### Mutation testing is an artifact, not a story

The mutants live in `mutants/`: twenty-four unified-diff patches, twelve
against `src/raft.hpp` and twelve against `src/exchange.hpp`, each with a
header naming the rule it breaks, the test expected to kill it, and
whether it is reconstructed from the narrative above or a candidate.
`mutants/run.sh` applies each patch to a scratch copy of `src/`, builds
both suites with `g++ -std=c++20 -O2`, runs them, and prints a
killed/survived table; it exits non-zero if any mutant survives, fails to
apply, or fails to build. CI runs it on every push as the `mutants` job,
so the kill table is regenerated by the code rather than remembered by
this file:

```
bash mutants/run.sh        # 24 / 24 killed, 185 s on 8 jobs (g++ 16.1)
```

The table records *how* each mutant died, not just that it did. All 24 die
as `killed/assert`: a named `FAIL` line and exit 1. That distinction is
load-bearing — a mutant that only ever kills by segfaulting has not shown
that any rule is pinned, it has shown that undefined behaviour is
reachable. Two mutants have been caught doing exactly that; one was
rejected and replaced, and one turned out to be a genuine assertion kill
whose diagnosis was being swallowed by a buffered `stdout` on abort. Both
are written up in `mutants/README.md`.

Building the gate found a third-round survivor. Dropping the term guard on
AppendEntries *replies* (a re-elected leader counting a success reply from
its own earlier term) passed all 4,550 seeded universes and the 450
exchange universes: the interleaving needs one reply delayed across two
leader changes with a partial replication in between, which no wire
configuration produces. `unit_stale_ae_reply` now scripts it, and the
ledger shows the resulting overwrite. The table also corrected a comment:
dropping the *term* half of the up-to-date vote check was not pinned by
the figure-8 test as `test_raft.cpp` claimed, nor by any seeded Raft
universe; the exchange chaos layer reached it (seed 14, n = 3), and the
new scripted test now pins it as well. Both are recorded in
`mutants/README.md`. The
first two rounds (a 1,000-universe suite, survivors at 8,000 and 3,950
universes) predate this repository's first commit and cannot be replayed;
the current table can.

## Why simulation instead of unit tests

Consensus bugs live in interleavings, not in functions. A unit test checks
the path you thought of; a seeded simulator generates the paths you did not,
and makes each one replayable. The two design rules that make this work:

1. The core is pure logic. Time is an argument, the network is a message
   list, randomness lives in the simulator. The same logic that runs under
   simulation will run under a real transport later, untouched.
2. Invariants are checked after every simulation tick (5 virtual ms), so a
   violation is caught within one tick of the state that produced it, not
   three seconds later. The ledger and canon oracles have memory, so
   nothing that happens inside a tick can hide behind re-convergence
   before the check.

### Seeds are portable

The simulator's own random draws (election timeouts, wire delays, the drop
and duplicate coin flips) go through a bounded reduction specified bit for
bit in `src/sim.hpp`: Lemire's nearly-divisionless method over a 128-bit
product built from 32-bit limbs for integers, and the top 53 bits of one
draw for [0, 1). They do not go through `std::uniform_int_distribution`,
whose algorithm is implementation-defined and differs across libstdc++,
libc++ and MSVC. So seed 8571 names the same universe on every toolchain in
the CI matrix, which includes a libc++ leg for exactly that reason. One
honest footnote on the switch: libstdc++ happens to implement the same
Lemire reduction, so on the g++ legs the integer draws are bit-identical to
what the standard distribution produced (checked directly: 0 of 10,000
draws differ) and the trajectories, liveness maxima included, are
unchanged. libc++ and MSVC reduce differently, so on those toolchains the
same seed used to name a different universe; now it names the g++ one.

Known limitation, on purpose: under a one-way link failure (leader can
send, cannot hear), basic Raft livelocks: the deaf leader's heartbeats
keep followers loyal while nothing can commit. The `oneway` scenario pins
the safe behavior (no commit without acks, no spurious depositions,
recovery on heal); the fix is the check-quorum extension, which belongs to
Phase C.

## Build and run

Windows (any g++ with C++20; MSYS2/WinLibs works):

```
./build.ps1
```

or directly:

```
g++ -std=c++20 -O2 -Wall -Wextra -static tests/test_raft.cpp -o test_raft
./test_raft
```

No dependencies. One translation unit. The full suite runs in about four
seconds.

## The state machine is an exchange (Phase B)

Committed log entries are orders; applying an entry is matching it. The
engine (`src/exchange.hpp`) is a fresh deterministic matching engine
written for this repository. It shares no code with
[hft-lob](https://github.com/Ronak-Mahajan/hft-lob), which is an ITCH 5.0
L2 book reconstructor with no matching path; the two answer different
questions (how fast can a book be rebuilt from a feed, versus can a book be
replicated so that every replica agrees). Here determinism is not a
performance trick, it is the correctness foundation. The simulator drives
the engine through per-node apply/restart hooks,
so the state-machine-safety oracle gets teeth: the books must agree at
every applied position, on every replica, through crashes, replays,
partitions, and reordered wires. Client-session
deduplication (exactly-once submission across leader failover) is
deliberately not here yet; today a resubmitted order id is rejected
deterministically, and sessions belong to Phase C.

The engine went through the same adversarial treatment as the consensus
core, and the first version failed it in instructive ways. A review found
the state hash was forgeable (level sentinels could be impersonated by
client-chosen order ids, giving two observably different books one hash;
fixed with count-prefix framing), that `operator>>` parsing consults the process
global locale (two identical binaries can diverge on the same bytes; fixed
with a hand-rolled digits-only parser and a canonical grammar), and that
unbounded quantities made volume arithmetic undefined behavior (fixed with
admission bounds). Mutation testing then showed the chaos layer has zero
matching-semantics killing power on its own (every replica runs the same
mutated binary and diverges identically, so all semantic coverage lives in
the unit layer) and that the unit layer had six blind spots, the
sharpest being the one-character sell-side mirror of a fully-pinned
buy-side rule. All twelve engine mutants now die, an uncrossed-book
invariant runs in every chaos universe, hostile commands flow through the
replicated path, and a golden-vector test with a compiled-in expected hash
turns any cross-build drift into a local unit failure.

## Roadmap

- **Phase C: chaos with numbers.** Client-visible latency percentiles
  (p50/p99 order-to-ack under leader failover), check-quorum for the
  one-way-partition livelock, client sessions with exactly-once submission,
  snapshotting/log compaction, and a real TCP transport behind the same
  message interface the simulator drives.

## License

MIT
