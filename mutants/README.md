# mutants/ -- the mutation gate

The README tells a mutation-testing story: twelve Raft mutants across two
rounds, twelve engine mutants, all killed. This directory is the artifact
behind it. Every mutant is a small unified-diff patch against
`src/raft.hpp` or `src/exchange.hpp`; `run.sh` applies each one to a
scratch copy, builds both suites, runs them, and prints a table. CI runs
it on every push (the `mutants` job in `.github/workflows/ci.yml`) and
fails the build on a survivor, an unappliable patch, or a mutant that does
not compile.

```
bash mutants/run.sh                       # whole table
bash mutants/run.sh mutants/raft-01-*.patch   # one mutant
CXX=clang++ JOBS=4 MUTANT_TIMEOUT=300 bash mutants/run.sh
```

`regen.sh` is the source of truth for the patches: each mutant is one
exact literal substitution, and the script diffs it against the current
`src/` to write the `.patch`. After any refactor of `src/` that makes
`run.sh` report `APPLY-FAILED`, edit the substitution and run
`bash mutants/regen.sh`. A substitution that matches nothing is an error,
so a mutant cannot silently stop mutating.

## Provenance, honestly

The repository's history is four commits with the suite arriving fully
formed, so the original patches and the earlier suite versions the README
numbers refer to (1,000 universes; survivors at 8,000 and 3,950) do not
exist anywhere and cannot be replayed. The patches here were reconstructed
from the README narrative and from the inline comment of the test that
pins each rule. Each header carries one of:

- **reconstructed**: the README or a test comment names this exact mutant
  (the figure-8 guard, blind truncation, the stale vote tally, the even
  quorum, the index tiebreak, the commit clamp; the sell-side mirror, the
  halved volume, the fill-stream hash).
- **candidate**: the rule and its pinning test are in the tree, but
  nothing in the tree says this exact mutant was one of the original
  twelve. They are shipped as full members of the gate: a rule with a
  test that does not fail when the rule is broken is not pinned.

## The table

Produced by `bash mutants/run.sh` (MinGW-w64 g++ 16.1, `-std=c++20 -O2`,
8 jobs, 185 s wall on this machine). CI regenerates it; if the numbers
below drift from what CI prints, CI is right and this file is stale.

### How a mutant died is part of the result

`killed` on its own is not a good enough answer. A suite that *detects* a
mutant prints a line beginning with `FAIL` and exits 1. Any other non-zero
exit is abnormal termination, which still stops the build but does so
through undefined behaviour rather than through a rule the suite knows how
to state. The runner separates the two:

| status | meaning |
|---|---|
| `killed/assert` | a `FAIL` line and exit 1. The named assertion is the kill. |
| `killed/assert+crash` | a `FAIL` line *and* an abnormal exit. The assertion is the kill; the crash is reported beside it, because a crash during a mutant run is nearly always a harness defect the mutant exposed. |
| `killed/crash` | abnormal exit, no `FAIL` line anywhere. Detected only by falling over. Does not fail the gate, but is called out under the table for investigation. |
| `killed/timeout` | ran past `$MUTANT_TIMEOUT`. Detected by not terminating. |
| `killed/nonzero` | exit 1 with no `FAIL` line: failed without saying why. |
| `SURVIVED` / `APPLY-FAILED` / `BUILD-FAILED` | as before; each fails the gate. |

All 24 mutants currently die in the strongest category, `killed/assert`.
That was not true of the first tables this directory produced; see item 4.

```
mutant                                         status               killed by                first failing line
------                                         ------               ---------                ------------------
exch-01-sell-at-best-bid-does-not-trade        killed/assert        test_exchange            FAIL: sell at best-bid price trades
exch-02-buy-at-best-ask-does-not-trade         killed/assert        test_exchange            FAIL: two fills across two levels
exch-03-volume-halved                          killed/assert        test_exchange            FAIL: volume equals traded quantity
exch-04-fill-price-left-out-of-hash            killed/assert        test_exchange            FAIL: hash separates identical books with different fill streams
exch-05-rejections-not-counted                 killed/assert        test_exchange            FAIL: even a rejected command moves the hash
exch-06-lifo-within-level                      killed/assert        test_exchange            FAIL: same price fills in arrival order
exch-07-open-id-collision-accepted             killed/assert        test_exchange            FAIL: rejected commands change no book state
exch-08-partial-fill-overfills-taker           killed/assert        test_exchange            FAIL: remainder walks to the next level
exch-09-filled-maker-id-not-released           killed/assert        test_exchange            FAIL: partially filled maker still resting
exch-10-price-admission-bound-dropped          killed/assert        test_exchange            FAIL: rejected commands change no book state
exch-11-trailing-bytes-accepted                killed/assert        test_exchange            FAIL: rejected commands change no book state
exch-12-cancel-leaves-order-resting            killed/assert        test_exchange            FAIL: cancelled order cannot trade; aggressor rests
raft-01-figure8-commit-older-term-by-count     killed/assert        test_raft                FAIL: committed entry overwritten at idx 1 (after rival leader overwrote index 1)
raft-02-blind-truncation-at-prev-index         killed/assert        test_raft                FAIL: stale retransmission must not truncate the log
raft-03-stale-vote-counted-across-terms        killed/assert        test_raft                FAIL: leader elected in term 2 on term-1 votes (no term-2 majority)
raft-04-election-quorum-half-is-majority       killed/assert        test_raft                FAIL: n=2: a candidate's own vote alone must not elect it
raft-05-commit-quorum-half-is-majority         killed/assert        test_raft                FAIL: quorum of a two-node cluster is two: no commit alone
raft-06-no-vote-reset-on-term-bump             killed/assert        test_raft test_exchange  FAIL: term bump resets the vote; term-6 candidate granted
raft-07-uptodate-index-tiebreak-dropped        killed/assert        test_raft test_exchange  FAIL: same last term, shorter log must be denied
raft-08-uptodate-term-half-dropped             killed/assert        test_raft test_exchange  FAIL: M leads term 4
raft-09-vote-grant-does-not-reset-timer        killed/assert        test_raft                FAIL: voter campaigned against the candidate it just voted for
raft-10-candidate-no-stepdown-on-equal-term-ae killed/assert        test_raft                FAIL: rival candidate did not step down on equal-term AppendEntries
raft-11-follower-commit-unclamped              killed/assert        test_raft                FAIL: commit index ran past the log on a partial-suffix AppendEntries
raft-12-stale-ae-reply-term-guard-dropped      killed/assert        test_raft                FAIL: leader committed a current-term entry on a stale reply from an earlier term

killed 24 / 24   survived 0   apply-failed 0   build-failed 0
  how they died: assertion 24, assertion+crash 0, crash only 0, timeout 0, unexplained exit 1 0
```

## What the table taught

Building the gate was itself a mutation round, and it found things the
narrative did not know. They are published here at the same size as the
kills.

1. **A survivor: `raft-12-stale-ae-reply-term-guard-dropped`.** Dropping
   `r.term != p_.current_term` from the AppendEntries-reply handler (a
   re-elected leader accepting success replies from its own earlier term)
   passed all 4,550 seeded Raft universes and all 450 exchange universes.
   The suite's `slow_wire` comment says "every stale-reply term guard is
   load-bearing" there; for this guard it was not. The interleaving needs
   one success reply delayed across two leader changes, with the leader's
   log truncated by an intervening leader and refilled with current-term
   entries at the indices the stale reply claims; 400 ms delays against
   150-300 ms timeouts never span that, and no scenario combines it with
   the partial replication. It is a real safety hole, not bookkeeping:
   with the mutant applied, the new scripted test `unit_stale_ae_reply`
   prints both `leader committed a current-term entry on a stale reply
   from an earlier term` and `committed entry overwritten at idx 3`. That
   test is the fix, in the repository's usual way: some interleavings get
   a guaranteed appearance.
2. **A wrong comment: `raft-08-uptodate-term-half-dropped`.**
   `test_raft.cpp` said the term half of the section 5.4.1 up-to-date
   check "is pinned by figure8". The first run of the table, before
   `unit_stale_ae_reply` existed, showed `unit_figure8` and every seeded
   Raft universe passing with that half removed; the only killer was the
   exchange chaos layer at seed 14, n = 3 (`committed entry overwritten
   at index 16 on node 1`), through the ledger invariant. The new scripted
   test happens to pin it too: its term-4 election needs F to grant on
   last term while holding the longer log, so the mutant now fails
   `M leads term 4` in `test_raft` as well. The comment was corrected.
3. **A rejected candidate.** "Cancel looks up the opposite side" was
   tried as the twelfth engine mutant. `unit_cancel` does not detect it
   (the cancelled order's level is erased as a side effect, so the
   observable outcome coincides with a correct cancel), and its only kill
   was a segmentation fault in the chaos layer through `front()` on an
   empty `std::deque`, i.e. undefined behaviour. A kill that depends on
   UB crashing is not one the gate may rely on, so the mutant was replaced
   by `exch-12-cancel-leaves-order-resting`, which `unit_cancel` kills
   deterministically. The gap is noted rather than closed: a cancel that
   erases the wrong side's level is currently unobservable at the unit
   layer.

4. **A kill that lied about itself: `raft-09-vote-grant-does-not-reset-timer`.**
   For several runs this mutant reached the table as
   `killed  test_raft  test_raft exit 127 (no FAIL line; crash or
   sanitizer?)`. It looked like the gate's one crash-kill. It was not.
   Rebuilding the mutant against the pre-fix tree and running the binary
   directly shows `test_raft` exiting **1** after printing exactly one
   named assertion, `FAIL: voter campaigned against the candidate it just
   voted for` — the scripted test written for this very rule. The mutant
   was always killed by an assertion. Two separate defects conspired to
   hide that:

   - **A harness defect the mutant exposed.** `bounds_failover` did
     `c.crash(c.leader())`. With the mutant applied, a voter that never
     resets its election timer can campaign and depose the leader between
     the election and that line, so `leader()` returns `-1` and `-1` went
     straight into `nodes_[id]` — an out-of-range `std::vector` index,
     i.e. undefined behaviour, not a detection. Being UB, it was not
     reproducible: on this machine's g++ 16.1 `-O2` build it silently did
     nothing, and on the build where the 127 appeared it aborted.
     `bounds_failover` and `bounds_heal` now `REQUIRE(ld >= 0, ...)` before
     using the id, and `sim::Cluster::crash` rejects an out-of-range node
     with a `violation` string the way `restart` already did. With the
     guard in place the mutant reports the rule it broke *and* the second
     named line `FAIL seed=225 n=5: leader deposed before the failover
     crash`.
   - **Buffered stdout.** When the abort did happen it happened in the
     liveness-bounds stage, minutes after the `FAIL` line was printed.
     `abort()` does not flush, so the buffer — and the assertion in it —
     was discarded, and the runner's `grep '^FAIL'` found nothing. Both
     suites now call `setvbuf(stdout, nullptr, _IONBF, 0)` in `main`, so a
     diagnosis printed before a crash survives the crash.

   The lesson generalises past this mutant: a mutation table that reports
   only *whether* a mutant died cannot tell a pinned rule from a lucky
   segfault, and the run that looks most like a success is the one worth
   opening. `run.sh` now reports the kill category per mutant and a
   `how they died` census under the table, and item 3's rejected candidate
   is the same principle applied a round earlier.

   **Windows-only artifact.** The `127` in the original symptom is a
   MinGW/MSYS detail worth recording, because it sends you looking for the
   wrong bug. A POSIX shell reports a child killed by `SIGABRT` as
   `128 + 6 = 134`; MinGW bash reports an aborted Windows process as
   `127`, the *same* code it uses for "command not found" and for a binary
   that cannot start because a DLL is missing. The first reading of
   `exit 127` on this platform is therefore usually "the build produced
   nothing runnable", which is not what had happened. `run.sh`'s
   `describe_rc` spells this out in the table rather than guessing, and
   labels the status a description rather than a diagnosis. The suites
   themselves are platform-independent; CI runs the gate on Linux, where
   the same abort would have surfaced as `139`/`134` and been read
   correctly on sight.

## Adding a mutant

Add an `emit` block to `regen.sh` (name, file, one literal perl
substitution, header with `rule`, `breaks`, `expected killer`,
`provenance`), run `bash mutants/regen.sh`, then `bash mutants/run.sh
mutants/<name>.patch`. If it survives, the suite has a hole: write the
test, do not delete the mutant.
