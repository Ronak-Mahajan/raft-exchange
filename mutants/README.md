# mutants/ -- the mutation gate

Twenty-four mutants: twelve against `src/raft.hpp` and twelve against
`src/exchange.hpp`. Each one is a small unified-diff patch that
re-introduces a bug the suite claims to catch, so that a rule the suite
does not actually pin shows up as a survivor. `run.sh` applies each patch
to a scratch copy, builds both suites, runs them, and prints a table. CI
runs it on every push (the `mutants` job in `.github/workflows/ci.yml`)
and fails the build on a survivor, an unappliable patch, or a mutant that
does not compile.

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

## Where each patch comes from

The earlier mutation rounds the top-level README describes (a
1,000-universe suite; survivors at 8,000 and 3,950 universes) predate this
repository's first commit, so their patches are not in the tree and their
numbers cannot be replayed. Everything below can. Each patch header carries
a `provenance` field, which opens with one of two words:

- **reconstructed**: the README or a test comment names this exact mutant
  (the figure-8 guard, blind truncation, the stale vote tally, the even
  quorum, the index tiebreak, the commit clamp; the sell-side mirror, the
  halved volume, the fill-stream hash).
- **candidate**: the rule and its pinning test are in the tree, and the
  mutant was written to check that the test does fail when the rule is
  broken.

Both are full members of the gate. A rule with a test that does not fail
when the rule is broken is not pinned.

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
| `killed/assert+crash` | a `FAIL` line *and* an abnormal exit. The assertion is the kill; the crash is reported beside it, because a crash during a mutant run usually points at a scenario the mutant steered into an unguarded state. |
| `killed/crash` | abnormal exit, no `FAIL` line anywhere. Detected only by falling over. Does not fail the gate, but is called out under the table for investigation. |
| `killed/timeout` | ran past `$MUTANT_TIMEOUT`. Detected by not terminating. |
| `killed/nonzero` | exit 1 with no `FAIL` line: failed without saying why. |
| `SURVIVED` / `APPLY-FAILED` / `BUILD-FAILED` | as before; each fails the gate. |

All 24 mutants die in the strongest category, `killed/assert`.

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

Running the gate is itself a mutation round, and four of its results say
more than a row of `killed` does. `run.sh` and several patch headers point
at the numbered items below.

1. **`raft-12-stale-ae-reply-term-guard-dropped` is reachable only by a
   scripted test.** Dropping `r.term != p_.current_term` from the
   AppendEntries-reply handler lets a re-elected leader accept success
   replies from its own earlier term. That mutant passes all 4,550 seeded
   Raft universes and all 450 exchange universes. The interleaving needs
   one success reply delayed across two leader changes, with the leader's
   log truncated by an intervening leader and refilled with current-term
   entries at the indices the stale reply claims; 400 ms delays against
   150-300 ms timeouts never span that, and no scenario combines it with
   the partial replication. It is a real safety hole, not bookkeeping:
   with the mutant applied, `unit_stale_ae_reply` prints both `leader
   committed a current-term entry on a stale reply from an earlier term`
   and `committed entry overwritten at idx 3`. Some interleavings deserve
   a guaranteed appearance rather than a probabilistic one, and this is
   one of them.
2. **`raft-08-uptodate-term-half-dropped` is killed from two layers, and
   neither is the figure-8 test.** Remove the term half of the section
   5.4.1 up-to-date check and `unit_figure8` passes, as does every seeded
   Raft universe. The exchange chaos layer catches it at seed 14, n = 3
   (`committed entry overwritten at index 16 on node 1`), through the
   ledger invariant. `unit_stale_ae_reply` pins it in `test_raft` as well:
   its term-4 election needs F to grant on last term while holding the
   longer log, so the mutant fails `M leads term 4` there too.
3. **One gap the gate does not close.** A cancel that looks up the
   opposite side is not observable at the unit layer. `unit_cancel` does
   not detect it, because the cancelled order's level is erased as a side
   effect and the observable outcome coincides with a correct cancel; the
   only signal anywhere is a segmentation fault in the chaos layer through
   `front()` on an empty `std::deque`, which is undefined behaviour rather
   than a detection, and a kill that depends on UB crashing is not one the
   gate may rely on. The twelfth engine mutant is therefore
   `exch-12-cancel-leaves-order-resting`, which `unit_cancel` kills
   deterministically. The opposite-side cancel is recorded here as an open
   gap.
4. **A named `FAIL` line only survives if the suite is built for it.**
   Both suites call `setvbuf(stdout, nullptr, _IONBF, 0)` in `main`, so a
   diagnosis printed before an abort survives the abort instead of dying
   in a buffer minutes later. The bounds stages guard their own inputs:
   `bounds_failover` and `bounds_heal` `REQUIRE` a non-negative leader id
   before using it, and `sim::Cluster::crash` rejects an out-of-range node
   with a `violation` string the way `restart` does. Both matter under
   mutation. With
   `raft-09-vote-grant-does-not-reset-timer` applied, a voter that never
   resets its election timer can depose the leader between an election and
   `c.crash(c.leader())`, so `leader()` returns `-1`; the guard turns that
   into the named line `FAIL seed=225 n=5: leader deposed before the
   failover crash` instead of an out-of-range `std::vector` index. Without
   these, a mutant killed by an assertion can reach the table looking like
   a segfault, and a table that reports only *whether* a mutant died
   cannot tell a pinned rule from a lucky crash.

   **Reading an abnormal exit on Windows.** A POSIX shell reports a child
   killed by `SIGABRT` as `128 + 6 = 134`. MinGW bash reports an aborted
   Windows process as `127`, the *same* code it uses for "command not
   found" and for a binary that cannot start because a DLL is missing, so
   `exit 127` on this platform reads as "the build produced nothing
   runnable" when it can equally mean an abort. `run.sh`'s `describe_rc`
   spells that out in the table rather than guessing, and labels the
   status a description rather than a diagnosis. The suites themselves are
   platform-independent; CI runs the gate on Linux, where the same abort
   surfaces as `134` or `139`.

## Adding a mutant

Add an `emit` block to `regen.sh` (name, file, one literal perl
substitution, header with `rule`, `breaks`, `expected killer`,
`provenance`), run `bash mutants/regen.sh`, then `bash mutants/run.sh
mutants/<name>.patch`. If it survives, the suite has a hole: write the
test, do not delete the mutant.
