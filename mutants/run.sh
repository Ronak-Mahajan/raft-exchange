#!/usr/bin/env bash
# mutants/run.sh - the mutation gate.
#
# For every mutants/*.patch: copy src/ and tests/ to a scratch directory,
# apply the patch, build both suites with $CXX $CXXFLAGS (default
# g++ -std=c++20 -O2), run them, and record the outcome.
#
# How a mutant died is reported, not just that it died, because the two
# are not worth the same. A suite that detects a mutant prints a line
# beginning with FAIL and returns 1. Any other non-zero status is abnormal
# termination -- an abort, a signal, or the shell's 126/127 -- which still
# kills the mutant but through undefined behaviour rather than through a
# named assertion, and the gate must not quietly bank that as a detection
# (see mutants/README.md, "What the table taught", items 3 and 4).
#
#   killed/assert        at least one suite printed a FAIL line and exited 1.
#                        The named assertion is the kill.
#   killed/assert+crash  a FAIL line AND an abnormal exit. The assertion is
#                        the kill; the crash is reported beside it because a
#                        crash in a mutant run is usually a harness defect.
#   killed/crash         abnormal exit with no FAIL line anywhere. Detected,
#                        but only by falling over: investigate before
#                        counting it.
#   killed/timeout       a suite ran past $MUTANT_TIMEOUT seconds (default
#                        600); detected by not terminating.
#   killed/nonzero       exit 1 with no FAIL line: the suite failed without
#                        saying why.
#   SURVIVED             both suites passed with the mutant applied.
#   APPLY-FAILED         the patch no longer matches src/ (regenerate it).
#   BUILD-FAILED         the mutant does not compile (a stillborn mutant
#                        proves nothing, so it fails the gate rather than
#                        counting as a kill).
#
# Both suites unbuffer stdout (setvbuf in main), so a FAIL line printed
# before an abort survives the abort and reaches this script. Without that,
# abort() discards the buffer and a genuine named kill reaches the table as
# an unexplained crash.
#
# The baseline (unmutated) suites are built and run first; if they fail,
# every "kill" would be meaningless, so the run aborts.
#
# Exit status is non-zero if any mutant survived, failed to apply, or
# failed to build. A crash-only kill does not fail the gate, but it is
# called out under the table. Mutants run in parallel ($JOBS, default
# nproc); the table is printed sorted, so output is deterministic.
#
# Usage:  bash mutants/run.sh            (from anywhere)
#         CXX=clang++ JOBS=2 bash mutants/run.sh
#         bash mutants/run.sh mutants/raft-01-figure8-commit-by-count.patch
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O2}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
TIMEOUT_S="${MUTANT_TIMEOUT:-600}"

# Name an abnormal exit status. Exit codes for abnormal termination are
# platform-specific: a POSIX shell reports a killed child as 128+signal
# (SIGABRT -> 134, SIGSEGV -> 139), while MinGW/MSYS bash reports an
# aborted Windows process as 127, the same code it uses for "command not
# found" and for a missing DLL. The status name below is therefore a
# description, not a diagnosis.
describe_rc() {
    case "$1" in
        126) echo "exit 126 (not executable)" ;;
        127) echo "exit 127 (abort, or the binary could not start: on MinGW"\
                  "an aborted process and a missing DLL share this code;"\
                  "a POSIX shell would report SIGABRT as 134)" ;;
        134) echo "SIGABRT (134)" ;;
        136) echo "SIGFPE (136)" ;;
        139) echo "SIGSEGV (139)" ;;
        *)
            if [ "$1" -gt 128 ]; then echo "signal $(( $1 - 128 )) ($1)"
            else echo "exit $1"; fi ;;
    esac
}

# ---- worker: one mutant ----------------------------------------------------
if [ "${1:-}" = "--one" ]; then
    patch_file="$2"
    work="$3"
    name="$(basename "$patch_file" .patch)"
    dir="$work/$name"
    mkdir -p "$dir/src" "$dir/tests"
    # Patches are LF; a CRLF checkout (Windows autocrlf) must not make every
    # hunk miss under --fuzz=0, so the scratch copy is normalised to LF.
    for f in "$ROOT"/src/*.hpp; do tr -d '\r' < "$f" > "$dir/src/$(basename "$f")"; done
    for f in "$ROOT"/tests/*.cpp; do tr -d '\r' < "$f" > "$dir/tests/$(basename "$f")"; done
    status=""
    killers=""
    detail=""
    if ! patch -p1 -d "$dir" --batch --forward --fuzz=0 -r - \
            < "$patch_file" > "$dir/apply.log" 2>&1; then
        status="APPLY-FAILED"
        detail="$(head -c 200 "$dir/apply.log" | tr '\n' ' ')"
    else
        for suite in test_raft test_exchange; do
            if ! "$CXX" $CXXFLAGS "$dir/tests/$suite.cpp" -o "$dir/$suite" \
                    > "$dir/$suite.build.log" 2>&1; then
                status="BUILD-FAILED"
                detail="$suite: $(grep -m1 -E 'error' "$dir/$suite.build.log" | head -c 200)"
                break
            fi
        done
        if [ -z "$status" ]; then
            asserted=0; crashed=0; timed_out=0; nonzero=0; crash_note=""
            for suite in test_raft test_exchange; do
                timeout "$TIMEOUT_S" "$dir/$suite" > "$dir/$suite.out" 2>&1
                rc=$?
                [ "$rc" -eq 0 ] && continue
                nonzero=1
                killers="$killers $suite"
                fail_line="$(grep -m1 -E '^FAIL' "$dir/$suite.out" | head -c 160)"
                if [ -n "$fail_line" ]; then
                    asserted=1
                    [ -n "$detail" ] || detail="$fail_line"
                fi
                if [ "$rc" -eq 124 ]; then
                    timed_out=1
                    [ -n "$detail" ] || detail="$suite ran past ${TIMEOUT_S}s"
                elif [ "$rc" -ne 1 ]; then
                    crashed=1
                    [ -n "$crash_note" ] || crash_note="$suite $(describe_rc "$rc")"
                    [ -n "$detail" ] || detail="$crash_note, no FAIL line"
                fi
            done
            if   [ "$timed_out" -eq 1 ]; then status="killed/timeout"
            elif [ "$asserted" -eq 1 ] && [ "$crashed" -eq 1 ]; then
                status="killed/assert+crash"
                detail="$detail  [also crashed: $crash_note]"
            elif [ "$asserted" -eq 1 ]; then status="killed/assert"
            elif [ "$crashed"  -eq 1 ]; then status="killed/crash"
            elif [ "$nonzero"  -eq 1 ]; then
                status="killed/nonzero"
                [ -n "$detail" ] || detail="exit 1 with no FAIL line"
            else status="SURVIVED"
            fi
        fi
    fi
    killers="${killers# }"
    printf '%s\t%s\t%s\t%s\n' "$name" "$status" "${killers:--}" "$detail" \
        > "$dir/result"
    exit 0
fi

# ---- driver ----------------------------------------------------------------
if [ "$#" -gt 0 ]; then
    patches=("$@")
else
    patches=("$ROOT"/mutants/*.patch)
fi
if [ "${#patches[@]}" -eq 0 ] || [ ! -f "${patches[0]}" ]; then
    echo "no patches found in $ROOT/mutants" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "mutants: ${#patches[@]} patches, compiler: $CXX $CXXFLAGS, jobs: $JOBS"
"$CXX" --version 2>/dev/null | head -1

# Baseline: the unmutated suites must pass, or every kill is noise.
echo "baseline: building and running the unmutated suites"
mkdir -p "$WORK/baseline"
base_ok=1
for suite in test_raft test_exchange; do
    if ! "$CXX" $CXXFLAGS "$ROOT/tests/$suite.cpp" -o "$WORK/baseline/$suite" \
            > "$WORK/baseline/$suite.build.log" 2>&1; then
        echo "baseline build FAILED for $suite:"
        cat "$WORK/baseline/$suite.build.log"
        base_ok=0
        break
    fi
    if ! timeout "$TIMEOUT_S" "$WORK/baseline/$suite" \
            > "$WORK/baseline/$suite.out" 2>&1; then
        echo "baseline run FAILED for $suite:"
        tail -n 40 "$WORK/baseline/$suite.out"
        base_ok=0
        break
    fi
done
if [ "$base_ok" -ne 1 ]; then
    echo "baseline does not pass; the mutant table would be meaningless. Aborting."
    exit 3
fi
echo "baseline: both suites pass"

printf '%s\n' "${patches[@]}" |
    xargs -P "$JOBS" -I{} bash "${BASH_SOURCE[0]}" --one "{}" "$WORK"

# ---- table -----------------------------------------------------------------
echo
printf '%-46s %-20s %-24s %s\n' "mutant" "status" "killed by" "first failing line"
printf '%-46s %-20s %-24s %s\n' "------" "------" "---------" "------------------"
survived=0; apply_failed=0; build_failed=0; killed=0; total=0
by_assert=0; by_crash=0; assert_plus_crash=0; by_timeout=0; by_nonzero=0
for p in "${patches[@]}"; do
    name="$(basename "$p" .patch)"
    res="$WORK/$name/result"
    if [ ! -f "$res" ]; then
        printf '%-46s %-20s %-24s %s\n' "$name" "NO-RESULT" "-" "worker produced no result"
        apply_failed=$((apply_failed + 1))
        total=$((total + 1))
        continue
    fi
    IFS=$'\t' read -r n status killers detail < "$res"
    printf '%-46s %-20s %-24s %s\n' "$n" "$status" "$killers" "${detail:0:90}"
    total=$((total + 1))
    case "$status" in
        killed/assert+crash) killed=$((killed+1)); assert_plus_crash=$((assert_plus_crash+1)) ;;
        killed/assert)       killed=$((killed+1)); by_assert=$((by_assert+1)) ;;
        killed/crash)        killed=$((killed+1)); by_crash=$((by_crash+1)) ;;
        killed/timeout)      killed=$((killed+1)); by_timeout=$((by_timeout+1)) ;;
        killed/nonzero)      killed=$((killed+1)); by_nonzero=$((by_nonzero+1)) ;;
        killed*)             killed=$((killed + 1)) ;;
        SURVIVED)            survived=$((survived + 1)) ;;
        APPLY-FAILED)        apply_failed=$((apply_failed + 1)) ;;
        BUILD-FAILED)        build_failed=$((build_failed + 1)) ;;
    esac
done
echo
echo "killed $killed / $total   survived $survived   apply-failed $apply_failed   build-failed $build_failed"
echo "  how they died: assertion $by_assert, assertion+crash $assert_plus_crash, crash only $by_crash, timeout $by_timeout, unexplained exit 1 $by_nonzero"

# Diagnostics for anything that is not a clean kill by a named assertion.
for p in "${patches[@]}"; do
    name="$(basename "$p" .patch)"
    res="$WORK/$name/result"
    [ -f "$res" ] || continue
    IFS=$'\t' read -r n status killers detail < "$res"
    case "$status" in
        SURVIVED)
            echo
            echo "== $n SURVIVED: both suites passed with the mutant applied =="
            grep -E '^# (rule|breaks|expected killer):' "$p" || true
            ;;
        APPLY-FAILED)
            echo
            echo "== $n APPLY-FAILED =="
            cat "$WORK/$name/apply.log"
            ;;
        BUILD-FAILED)
            echo
            echo "== $n BUILD-FAILED =="
            for f in "$WORK/$name"/*.build.log; do
                [ -f "$f" ] && head -n 30 "$f"
            done
            ;;
        killed/crash)
            echo
            echo "== $n killed only by CRASHING: $detail =="
            echo "A kill that depends on undefined behaviour is not one the gate"
            echo "may rely on. Find the named assertion, or fix the harness."
            for f in "$WORK/$name"/test_*.out; do
                [ -f "$f" ] && { echo "-- $(basename "$f") (tail) --"; tail -n 15 "$f"; }
            done
            ;;
        killed/assert+crash)
            echo
            echo "== $n was killed by an assertion AND then crashed =="
            echo "$detail"
            echo "The assertion is the kill. The crash is almost always a harness"
            echo "defect the mutant exposed; fix it so the table stays readable."
            ;;
        killed/nonzero)
            echo
            echo "== $n exited non-zero with no FAIL line: $detail =="
            ;;
    esac
done

if [ "$survived" -gt 0 ] || [ "$apply_failed" -gt 0 ] || [ "$build_failed" -gt 0 ]; then
    exit 1
fi
exit 0
