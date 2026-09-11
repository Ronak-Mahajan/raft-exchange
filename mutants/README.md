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

Produced by `bash mutants/run.sh` on the commit that introduced this
directory (MinGW-w64 g++ 16.1, `-std=c++20 -O2`, 8 jobs, 77 s). CI
regenerates it; if the numbers below drift from what CI prints, CI is
right and this file is stale.

```
mutant                                       status   killed by                first failing line
raft-01-figure8-commit-older-term-by-count   killed   test_raft                committed entry overwritten at idx 1 (after rival leader overwrote index 1)
raft-02-blind-truncation-at-prev-index       killed   test_raft                stale retransmission must not truncate the log
raft-03-stale-vote-counted-across-terms      killed   test_raft                leader elected in term 2 on term-1 votes (no term-2 majority)
raft-04-election-quorum-half-is-majority     killed   test_raft                n=2: a candidate's own vote alone must not elect it
raft-05-commit-quorum-half-is-majority       killed   test_raft                quorum of a two-node cluster is two: no commit alone
raft-06-no-vote-reset-on-term-bump           killed   test_raft test_exchange  term bump resets the vote; term-6 candidate granted
raft-07-uptodate-index-tiebreak-dropped      killed   test_raft test_exchange  same last term, shorter log must be denied
raft-08-uptodate-term-half-dropped           killed   test_raft test_exchange  M leads term 4
raft-09-vote-grant-does-not-reset-timer      killed   test_raft                voter campaigned against the candidate it just voted for
raft-10-candidate-no-stepdown-on-equal-term-ae killed test_raft                rival candidate did not step down on equal-term AppendEntries
raft-11-follower-commit-unclamped            killed   test_raft                commit index ran past the log on a partial-suffix AppendEntries
raft-12-stale-ae-reply-term-guard-dropped    killed   test_raft                leader committed a current-term entry on a stale reply from an earlier term
exch-01-sell-at-best-bid-does-not-trade      killed   test_exchange            sell at best-bid price trades
exch-02-buy-at-best-ask-does-not-trade       killed   test_exchange            two fills across two levels
exch-03-volume-halved                        killed   test_exchange            volume equals traded quantity
exch-04-fill-price-left-out-of-hash          killed   test_exchange            hash separates identical books with different fill streams
exch-05-rejections-not-counted               killed   test_exchange            even a rejected command moves the hash
exch-06-lifo-within-level                    killed   test_exchange            same price fills in arrival order
exch-07-open-id-collision-accepted           killed   test_exchange            rejected commands change no book state
exch-08-partial-fill-overfills-taker         killed   test_exchange            remainder walks to the next level
exch-09-filled-maker-id-not-released         killed   test_exchange            partially filled maker still resting
exch-10-price-admission-bound-dropped        killed   test_exchange            rejected commands change no book state
exch-11-trailing-bytes-accepted              killed   test_exchange            rejected commands change no book state
exch-12-cancel-leaves-order-resting          killed   test_exchange            cancelled order cannot trade; aggressor rests

killed 24 / 24   survived 0   apply-failed 0   build-failed 0
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

## Adding a mutant

Add an `emit` block to `regen.sh` (name, file, one literal perl
substitution, header with `rule`, `breaks`, `expected killer`,
`provenance`), run `bash mutants/regen.sh`, then `bash mutants/run.sh
mutants/<name>.patch`. If it survives, the suite has a hole: write the
test, do not delete the mutant.
