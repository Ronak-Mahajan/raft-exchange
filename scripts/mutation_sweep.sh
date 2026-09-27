#!/usr/bin/env bash
# scripts/mutation_sweep.sh - the generated mutation sweep.
#
# mutants/ holds hand-written mutants, each aimed at one rule. The sweep
# asks the complementary question mechanically: scripts/sweep_mutants.pl
# generates every relational-operator swap and every single-statement
# deletion inside the function bodies of src/raft.hpp and src/exchange.hpp,
# and this script builds and runs the suites against each one:
#
#   killed/assert   a suite printed a FAIL line and exited 1
#   killed/crash    a suite exited abnormally with no FAIL line
#   killed/timeout  a suite ran past $SWEEP_TIMEOUT seconds (default 120)
#   equivalent      both suites pass, and scripts/sweep_equivalent.txt
#                   gives the reason no test can tell the mutant apart
#   SURVIVED        both suites pass and the mutant is not annotated
#   stillborn       the mutant does not compile. Builds add
#                   -Werror=return-type, so a deleted final return (which
#                   leaves undefined behaviour, not a program) is
#                   stillborn rather than a crash.
#
# The run exits 1 on any SURVIVED mutant and on any stale annotation: a
# key in sweep_equivalent.txt that is no longer generated, or whose mutant
# the suites kill. It exits 2 if the unmutated suites fail.
#
# Suites run with SUITE_FAIL_FAST=1, so a kill costs one build and a run
# up to the first FAIL line. A raft.hpp mutant builds test_raft first and
# test_exchange only if test_raft does not assert; test_raft does not
# include exchange.hpp, so an exchange.hpp mutant runs test_exchange alone.
#
# Usage:  bash scripts/mutation_sweep.sh              (from anywhere)
#         bash scripts/mutation_sweep.sh --list       (generate, run nothing)
#         CXX=clang++ JOBS=4 bash scripts/mutation_sweep.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2} -Werror=return-type"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
TIMEOUT_S="${SWEEP_TIMEOUT:-120}"
FILES=(src/raft.hpp src/exchange.hpp)
ANNOT="$ROOT/scripts/sweep_equivalent.txt"

# ---- worker: one mutant ----------------------------------------------------
# Invoked by xargs as: mutation_sweep.sh --one WORK KEY FILE
if [ "${1:-}" = "--one" ]; then
    work="$2"; key="$3"; file="$4"
    dir="$work/run/$key"
    mkdir -p "$dir/src" "$dir/tests"
    for f in "$ROOT"/src/*.hpp; do tr -d '\r' < "$f" > "$dir/src/$(basename "$f")"; done
    for f in "$ROOT"/tests/*.cpp; do tr -d '\r' < "$f" > "$dir/tests/$(basename "$f")"; done
    cp "$work/gen/$key/$file" "$dir/$file"
    case "$file" in
        src/exchange.hpp) suites="test_exchange" ;;
        *)                suites="test_raft test_exchange" ;;
    esac
    status=""; killer="-"; detail=""; crash_note=""
    for suite in $suites; do
        if ! $CXX $CXXFLAGS "$dir/tests/$suite.cpp" -o "$dir/$suite" \
                > "$dir/$suite.build.log" 2>&1; then
            status="stillborn"
            detail="$(grep -m1 -E 'error' "$dir/$suite.build.log" | sed 's/.*error: //' | head -c 120)"
            break
        fi
        # The subshell absorbs bash's "Segmentation fault" job message; the
        # status (128 + signal) still comes through.
        ( SUITE_FAIL_FAST=1 timeout "$TIMEOUT_S" "$dir/$suite" \
              > "$dir/$suite.out" 2>&1; exit $? ) 2>/dev/null
        rc=$?
        [ "$rc" -eq 0 ] && continue
        fail_line="$(grep -m1 -E '^FAIL' "$dir/$suite.out" | head -c 150)"
        if [ -n "$fail_line" ] && [ "$rc" -eq 1 ]; then
            status="killed/assert"; killer="$suite"; detail="$fail_line"
            break
        fi
        if [ "$rc" -eq 124 ]; then
            status="killed/timeout"; killer="$suite"
            detail="$suite ran past ${TIMEOUT_S}s"
            break
        fi
        # An abnormal exit: keep looking for a named kill in the next suite.
        if [ -z "$crash_note" ]; then
            crash_note="$suite exit $rc, no FAIL line"; killer="$suite"
        fi
    done
    if [ -z "$status" ]; then
        if [ -n "$crash_note" ]; then status="killed/crash"; detail="$crash_note"
        else status="SURVIVED"; fi
    fi
    printf '%s\t%s\t%s\n' "$status" "$killer" "$detail" > "$dir/result"
    exit 0
fi

# ---- driver ----------------------------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$ROOT" || exit 2
if ! perl scripts/sweep_mutants.pl "$WORK/gen" "${FILES[@]}" > "$WORK/raw.tsv"; then
    echo "sweep: mutant generation failed" >&2
    exit 2
fi
sort -t$'\t' -k2,2 -k3,3n -k1,1 "$WORK/raw.tsv" > "$WORK/manifest.tsv"
total=$(wc -l < "$WORK/manifest.tsv" | tr -d ' ')
if [ "${1:-}" = "--list" ]; then
    cat "$WORK/manifest.tsv"
    echo "sweep: $total mutants"
    exit 0
fi

echo "sweep: $total mutants of ${FILES[*]}"
echo "compiler: $CXX $CXXFLAGS, jobs: $JOBS, timeout: ${TIMEOUT_S}s per suite"
"$CXX" --version 2>/dev/null | head -1

echo "baseline: building and running the unmutated suites"
mkdir -p "$WORK/baseline"
for suite in test_raft test_exchange; do
    if ! $CXX $CXXFLAGS "$ROOT/tests/$suite.cpp" -o "$WORK/baseline/$suite" \
            > "$WORK/baseline/$suite.build.log" 2>&1; then
        echo "baseline build FAILED for $suite:"; cat "$WORK/baseline/$suite.build.log"
        exit 2
    fi
    if ! SUITE_FAIL_FAST=1 timeout "$TIMEOUT_S" "$WORK/baseline/$suite" \
            > "$WORK/baseline/$suite.out" 2>&1; then
        echo "baseline run FAILED for $suite:"; tail -n 40 "$WORK/baseline/$suite.out"
        exit 2
    fi
done
echo "baseline: both suites pass"

cut -f1,2 "$WORK/manifest.tsv" | tr '\t' ' ' |
    xargs -P "$JOBS" -L 1 bash "${BASH_SOURCE[0]}" --one "$WORK"

# ---- report ----------------------------------------------------------------
declare -A reason
if [ -f "$ANNOT" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in ''|'#'*) continue ;; esac
        k="${line%%[[:space:]]*}"
        r="${line#"$k"}"
        reason[$k]="${r#"${r%%[![:space:]]*}"}"
    done < "$ANNOT"
fi

by_assert=0; by_crash=0; by_timeout=0; equivalent=0; survived=0
stillborn=0; no_result=0; stale=0
declare -A generated
: > "$WORK/survivors"; : > "$WORK/equivalents"; : > "$WORK/others"; : > "$WORK/stale"
echo
printf '%-10s %-15s %-14s %-24s %s\n' "key" "status" "killed by" "where" "mutant"
printf '%-10s %-15s %-14s %-24s %s\n' "---" "------" "---------" "-----" "------"
while IFS=$'\t' read -r key file line op desc; do
    generated[$key]=1
    res="$WORK/run/$key/result"
    if [ -f "$res" ]; then
        IFS=$'\t' read -r status killer detail < "$res"
    else
        status="NO-RESULT"; killer="-"; detail="worker produced no result"
    fi
    annotated=0; [ -n "${reason[$key]+x}" ] && annotated=1
    case "$status" in
        SURVIVED)
            if [ "$annotated" -eq 1 ]; then
                status="equivalent"; equivalent=$((equivalent + 1))
                printf '%s %s:%s %s: %s\n    %s\n' "$key" "$file" "$line" "$op" "$desc" \
                    "${reason[$key]}" >> "$WORK/equivalents"
            else
                survived=$((survived + 1))
                printf '%s %s:%s %s: %s\n' "$key" "$file" "$line" "$op" "$desc" >> "$WORK/survivors"
            fi ;;
        killed/assert)  by_assert=$((by_assert + 1)) ;;
        killed/crash)   by_crash=$((by_crash + 1)) ;;
        killed/timeout) by_timeout=$((by_timeout + 1)) ;;
        stillborn)      stillborn=$((stillborn + 1)) ;;
        *)              no_result=$((no_result + 1)) ;;
    esac
    case "$status" in
        killed/crash|killed/timeout|NO-RESULT)
            printf '%s %s %s:%s %s: %s\n    %s\n' "$key" "$status" "$file" "$line" "$op" \
                "$desc" "$detail" >> "$WORK/others" ;;
    esac
    if [ "$annotated" -eq 1 ] && [ "$status" != "equivalent" ]; then
        stale=$((stale + 1))
        printf '%s is annotated as equivalent but is %s: %s:%s %s: %s\n' \
            "$key" "$status" "$file" "$line" "$op" "$desc" >> "$WORK/stale"
    fi
    shown="$op: $desc"
    [ "$status" = "killed/assert" ] && shown="$shown  [$detail]"
    printf '%-10s %-15s %-14s %-24s %s\n' "$key" "$status" "$killer" \
        "$(basename "$file"):$line" "${shown:0:160}"
done < "$WORK/manifest.tsv"
for k in "${!reason[@]}"; do
    if [ -z "${generated[$k]+x}" ]; then
        stale=$((stale + 1))
        echo "$k is annotated in scripts/sweep_equivalent.txt but no longer generated" >> "$WORK/stale"
    fi
done

if [ -s "$WORK/equivalents" ]; then
    echo; echo "== equivalent (annotated in scripts/sweep_equivalent.txt) =="
    cat "$WORK/equivalents"
fi
if [ -s "$WORK/others" ]; then
    echo; echo "== killed without a named assertion =="
    cat "$WORK/others"
fi
if [ -s "$WORK/survivors" ]; then
    echo; echo "== SURVIVED: both suites pass and no annotation explains why =="
    cat "$WORK/survivors"
    echo "Write the test that kills each one, or, if no test can, add its key"
    echo "and the reason to scripts/sweep_equivalent.txt."
fi
if [ -s "$WORK/stale" ]; then
    echo; echo "== stale annotations =="
    sort "$WORK/stale"
fi

echo
echo "sweep: $total mutants: $by_assert killed by a named assertion, $by_crash by crash only,"
echo "       $by_timeout by timeout, $equivalent equivalent (annotated), $survived survived,"
echo "       $stillborn stillborn"
[ "$no_result" -eq 0 ] || echo "       $no_result produced no result"
[ "$stale" -eq 0 ] || echo "       $stale stale annotations"

if [ "$survived" -gt 0 ] || [ "$stale" -gt 0 ] || [ "$no_result" -gt 0 ]; then
    exit 1
fi
exit 0
