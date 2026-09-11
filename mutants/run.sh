#!/usr/bin/env bash
# mutants/run.sh - the mutation gate.
#
# For every mutants/*.patch: copy src/ and tests/ to a scratch directory,
# apply the patch, build both suites with $CXX $CXXFLAGS (default
# g++ -std=c++20 -O2), run them, and record the outcome:
#
#   killed        at least one suite exited non-zero (a FAIL line, an
#                 assertion, a crash) -- the mutant was detected
#   killed/timeout a suite ran past $MUTANT_TIMEOUT seconds (default 600);
#                 detected, but by not terminating rather than by a FAIL
#   SURVIVED      both suites passed with the mutant applied
#   APPLY-FAILED  the patch no longer matches src/ (regenerate it)
#   BUILD-FAILED  the mutant does not compile (a stillborn mutant proves
#                 nothing, so it fails the gate rather than counting as a
#                 kill)
#
# The baseline (unmutated) suites are built and run first; if they fail,
# every "kill" would be meaningless, so the run aborts.
#
# Exit status is non-zero if any mutant survived, failed to apply, or
# failed to build. Mutants run in parallel ($JOBS, default nproc); the
# table is printed sorted, so output is deterministic.
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
            for suite in test_raft test_exchange; do
                timeout "$TIMEOUT_S" "$dir/$suite" > "$dir/$suite.out" 2>&1
                rc=$?
                if [ "$rc" -eq 124 ]; then
                    status="killed/timeout"
                    killers="$killers $suite"
                elif [ "$rc" -ne 0 ]; then
                    [ "$status" = "killed/timeout" ] || status="killed"
                    killers="$killers $suite"
                    if [ -z "$detail" ]; then
                        detail="$(grep -m1 -E '^FAIL' "$dir/$suite.out" | head -c 200)"
                        [ -n "$detail" ] || detail="$suite exit $rc (no FAIL line; crash or sanitizer?)"
                    fi
                fi
            done
            [ -n "$status" ] || status="SURVIVED"
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
printf '%-40s %-15s %-24s %s\n' "mutant" "status" "killed by" "first failing line"
printf '%-40s %-15s %-24s %s\n' "------" "------" "---------" "------------------"
survived=0; apply_failed=0; build_failed=0; killed=0; total=0
for p in "${patches[@]}"; do
    name="$(basename "$p" .patch)"
    res="$WORK/$name/result"
    if [ ! -f "$res" ]; then
        printf '%-40s %-15s %-24s %s\n' "$name" "NO-RESULT" "-" "worker produced no result"
        apply_failed=$((apply_failed + 1))
        total=$((total + 1))
        continue
    fi
    IFS=$'\t' read -r n status killers detail < "$res"
    printf '%-40s %-15s %-24s %s\n' "$n" "$status" "$killers" "${detail:0:80}"
    total=$((total + 1))
    case "$status" in
        killed*)      killed=$((killed + 1)) ;;
        SURVIVED)     survived=$((survived + 1)) ;;
        APPLY-FAILED) apply_failed=$((apply_failed + 1)) ;;
        BUILD-FAILED) build_failed=$((build_failed + 1)) ;;
    esac
done
echo
echo "killed $killed / $total   survived $survived   apply-failed $apply_failed   build-failed $build_failed"

# Diagnostics for anything that is not a clean kill.
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
    esac
done

if [ "$survived" -gt 0 ] || [ "$apply_failed" -gt 0 ] || [ "$build_failed" -gt 0 ]; then
    exit 1
fi
exit 0
