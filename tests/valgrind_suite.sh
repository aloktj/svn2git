#!/usr/bin/env bash
#
# valgrind_suite.sh — memcheck regression gate for the svn2git validation
# tooling. Three legs, each under valgrind:
#
#   1. svn2git-unit-tests         pure C++ paths with fake command runners
#   2. svn2git-integration-tests  real svn/git fixture repositories
#   3. the full CLI surface       via smoke_test.sh with SVN2GIT_TEST_WRAPPER
#                                 (exercises main(), argument parsing and
#                                 report writing — paths the Catch2 binaries
#                                 never reach)
#
# Child processes (svn, git) are deliberately NOT traced — only our own
# binaries are audited. Zero-tolerance gate: the suite fails unless every
# audited process exits with ZERO bytes on the heap ("All heap blocks
# were freed") — definite, indirect, possible and still-reachable blocks
# all count as failures.
#
# Usage:  tests/valgrind_suite.sh [--quick] [build-dir]
#           --quick     run only leg 1 (fast local iteration)
#           build-dir   defaults to <repo>/build

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

QUICK=
BUILD="$SCRIPT_DIR/../build"
for arg in "$@"; do
    case "$arg" in
    --quick) QUICK=1 ;;
    -*) echo "usage: $0 [--quick] [build-dir]" >&2; exit 2 ;;
    *) BUILD="$arg" ;;
    esac
done
BUILD="$(cd "$BUILD" 2>/dev/null && pwd)" || { echo "error: build dir not found" >&2; exit 2; }

command -v valgrind >/dev/null || { echo "error: valgrind not installed" >&2; exit 2; }

# --errors-for-leak-kinds=all makes EVERY leak kind (definite, indirect,
# possible, reachable) count as an error, so --error-exitcode=99 turns
# any byte left on the heap into a failing exit code.
VG_OPTS="--leak-check=full --show-leak-kinds=all \
--errors-for-leak-kinds=all --error-exitcode=99 \
--child-silent-after-fork=yes"

LOGS="$(mktemp -d)"
FAILED=0

# check_logs <leg-name> <log-glob...> — scan valgrind logs for trouble
check_logs() {
    local name="$1"
    shift
    local bad=0 log
    for log in "$@"; do
        [ -f "$log" ] || continue
        # Fork children that exec svn/git leave truncated logs with no
        # summary — only the tool's own processes produce a verdict.
        grep -q 'ERROR SUMMARY' "$log" || continue
        # Zero tolerance: the process must leave nothing on the heap at
        # all — valgrind then prints "no leaks are possible".
        if ! grep -q 'ERROR SUMMARY: 0 errors' "$log" \
           || ! grep -q 'no leaks are possible' "$log"; then
            bad=1
            echo "      $(basename "$log"):"
            grep -E 'ERROR SUMMARY|in use at exit|(definitely|indirectly|possibly) lost:|still reachable:' \
                "$log" | sed 's/^/        /'
        fi
    done
    if [ "$bad" -eq 0 ]; then
        echo "PASS  $name — 0 errors, 0 bytes on the heap at exit"
    else
        echo "FAIL  $name — memcheck findings above (logs: $LOGS)"
        FAILED=1
    fi
}

echo "== Leg 1: unit tests under valgrind"
# shellcheck disable=SC2086 — VG_OPTS is intentionally word-split
valgrind $VG_OPTS --log-file="$LOGS/unit.vg" \
    "$BUILD/tests/svn2git-unit-tests" >"$LOGS/unit.out" 2>&1
rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL  unit tests exited $rc"; tail -5 "$LOGS/unit.out"; FAILED=1; }
check_logs "unit tests" "$LOGS/unit.vg"

if [ -z "$QUICK" ]; then
    echo "== Leg 2: integration tests under valgrind"
    # shellcheck disable=SC2086
    valgrind $VG_OPTS --log-file="$LOGS/integration.vg" \
        "$BUILD/tests/svn2git-integration-tests" >"$LOGS/integration.out" 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "FAIL  integration tests exited $rc"; tail -5 "$LOGS/integration.out"; FAILED=1; }
    check_logs "integration tests" "$LOGS/integration.vg"

    echo "== Leg 3: full CLI surface (smoke test) under valgrind"
    # %p = PID: one log per CLI invocation, scanned collectively below.
    if SVN2GIT_TEST_WRAPPER="valgrind $VG_OPTS --log-file=$LOGS/cli-%p.vg" \
        "$SCRIPT_DIR/smoke_test.sh" "$BUILD/svn2git-validate" >"$LOGS/smoke.out" 2>&1; then
        echo "PASS  smoke test 30/30 under valgrind"
    else
        echo "FAIL  smoke test failed under valgrind:"
        grep '^FAIL' "$LOGS/smoke.out" | sed 's/^/      /'
        FAILED=1
    fi
    check_logs "CLI invocations ($(ls "$LOGS"/cli-*.vg 2>/dev/null | wc -l) logs)" "$LOGS"/cli-*.vg
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "== RESULT: memcheck clean"
    rm -rf "$LOGS"
else
    echo "== RESULT: memcheck FAILED — logs kept in $LOGS"
fi
exit "$FAILED"
