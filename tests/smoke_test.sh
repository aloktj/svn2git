#!/usr/bin/env bash
#
# smoke_test.sh — one-shot end-to-end exercise of every svn2git-validate
# command against a real, locally created SVN repository.
#
# What it builds:
#   - an SVN repo with trunk, a cheap-copied branch (release-1.0) and a tag
#   - a faithful git mirror of that layout (with svn2git metadata trailers)
#   - a mirror with a file deleted from the branch  (cheap-copy loss)
#   - a mirror with a file's content tampered       (corruption)
#
# What it checks, per command: the exit code (0 ok / 1 validation failure /
# 2 usage error) and, where it matters, the report text.
#
# Usage:  tests/smoke_test.sh [path-to-svn2git-validate]
#         (default binary: <repo>/build/svn2git-validate)
#
# SVN2GIT_TEST_WRAPPER, when set, is prefixed (word-split) to every binary
# invocation — e.g. SVN2GIT_TEST_WRAPPER="valgrind --error-exitcode=99"
# runs the whole suite under memcheck. Exit-code assertions still hold
# because valgrind forwards the program's exit code unless it finds errors.

set -u

WRAPPER="${SVN2GIT_TEST_WRAPPER:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${1:-$SCRIPT_DIR/../build/svn2git-validate}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

if [ ! -x "$BIN" ]; then
    echo "error: binary not found: $BIN (build first: cmake --build build)" >&2
    exit 2
fi
for tool in svn svnadmin git; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' not installed" >&2; exit 2; }
done

WORK="$(mktemp -d)"
# KEEP=1 tests/smoke_test.sh  preserves the workspace for debugging
trap '[ -n "${KEEP:-}" ] && echo "workspace kept: $WORK" || rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
N=0

# run <name> <expected-exit-code> <command...>
# Runs the command in its own directory (the tool writes report files to
# the CWD) and records pass/fail. Output lands in $LAST_DIR/std{out,err}.log.
run() {
    local name="$1" expected="$2"
    shift 2
    N=$((N + 1))
    LAST_DIR="$WORK/runs/$(printf '%02d' "$N")-$name"
    mkdir -p "$LAST_DIR"
    (cd "$LAST_DIR" && "$@") >"$LAST_DIR/stdout.log" 2>"$LAST_DIR/stderr.log"
    local actual=$?
    if [ "$actual" -eq "$expected" ]; then
        PASS=$((PASS + 1))
        printf 'PASS  %-42s (exit %d)\n' "$name" "$actual"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL  %-42s (exit %d, expected %d)\n' "$name" "$actual" "$expected"
        sed 's/^/      | /' "$LAST_DIR/stdout.log" "$LAST_DIR/stderr.log" | tail -20
    fi
}

# expect_in_output <pattern> — grep the last run's stdout for evidence
expect_in_output() {
    if grep -q "$1" "$LAST_DIR/stdout.log"; then
        PASS=$((PASS + 1))
        printf 'PASS  %-42s (output contains "%s")\n' "$(basename "$LAST_DIR" | cut -d- -f2-)" "$1"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL  output of %s lacks "%s"\n' "$(basename "$LAST_DIR")" "$1"
    fi
}

# expect_file <path> — assert a non-empty artifact was produced
expect_file() {
    if [ -s "$1" ]; then
        PASS=$((PASS + 1))
        printf 'PASS  artifact %s exists (%s bytes)\n' "$(basename "$1")" "$(wc -c <"$1")"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL  artifact missing or empty: %s\n' "$1"
    fi
}

echo "== 1. Building fixture SVN repository (trunk + cheap-copied branch + tag)"

svnadmin create "$WORK/svnrepo"
URL="file://$WORK/svnrepo"

CO="$WORK/checkout"
svn checkout -q "$URL" "$CO"
mkdir -p "$CO/trunk/src" "$CO/trunk/docs" "$CO/branches" "$CO/tags"
cat >"$CO/trunk/src/main.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("hello\n"); return 0; }
EOF
echo "user manual v1" >"$CO/trunk/docs/manual.txt"
echo "build notes"    >"$CO/trunk/docs/build notes.txt"   # space in name on purpose
(cd "$CO" && svn add -q trunk branches tags && svn commit -q -m "r1: initial trunk import")

echo "user manual v2" >"$CO/trunk/docs/manual.txt"
(cd "$CO" && svn commit -q -m "r2: revise manual")

# The cheap copy — the exact operation that historically loses files.
svn copy -q -m "r3: branch release-1.0 (cheap copy)" "$URL/trunk" "$URL/branches/release-1.0"

(cd "$CO" && svn update -q)
echo "release-only hotfix" >"$CO/branches/release-1.0/src/hotfix.c"
(cd "$CO" && svn add -q branches/release-1.0/src/hotfix.c \
          && svn commit -q -m "r4: hotfix on release branch")

svn copy -q -m "r5: tag v1.0" "$URL/trunk" "$URL/tags/v1.0"

SVN_AUTHOR="$(svn log -q -l1 "$URL" | awk -F'|' 'NR==2 {gsub(/ /,"",$2); print $2}')"
echo "   SVN repo at $URL (author: $SVN_AUTHOR)"

echo "== 2. Building git mirrors (faithful, file-deleted, content-tampered)"

sync_tree() { # sync_tree <git-worktree> <svn-path>  — replace content with an svn export
    local tree="$1" path="$2"
    find "$tree" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
    # Drop the stat cache: under core.checkstat=minimal a same-size rewrite
    # within the same second is invisible to `git add`, which would silently
    # skip a fixture commit. A fresh index rehashes everything from disk.
    rm -f "$tree/.git/index"
    svn export -q --force "$URL/$path" "$tree"
}

MIRROR="$WORK/mirror"
git init -q -b master "$MIRROR"
git -C "$MIRROR" config user.name "Migration Bot"
git -C "$MIRROR" config user.email "bot@example.com"

sync_tree "$MIRROR" "trunk@1"
git -C "$MIRROR" add -A
git -C "$MIRROR" commit -q -m "r1: initial trunk import

svn path=/trunk/; revision=1"

sync_tree "$MIRROR" "trunk"
git -C "$MIRROR" add -A
git -C "$MIRROR" commit -q -m "r2: revise manual

svn path=/trunk/; revision=2"

git -C "$MIRROR" checkout -q -b release-1.0
sync_tree "$MIRROR" "branches/release-1.0"
git -C "$MIRROR" add -A
git -C "$MIRROR" commit -q -m "r4: hotfix on release branch

svn path=/branches/release-1.0/; revision=4"
git -C "$MIRROR" checkout -q master
git -C "$MIRROR" tag v1.0    # tags/v1.0 was copied from trunk HEAD

LOSSY="$WORK/mirror-lossy"     # simulates cheap-copy data loss
cp -r "$MIRROR" "$LOSSY"
git -C "$LOSSY" checkout -q release-1.0
git -C "$LOSSY" rm -q docs/manual.txt
git -C "$LOSSY" commit -q -m "simulated cheap-copy loss"
git -C "$LOSSY" checkout -q master

TAMPERED="$WORK/mirror-tampered"   # simulates content corruption
cp -r "$MIRROR" "$TAMPERED"
echo "silently corrupted" >>"$TAMPERED/src/main.c"
git -C "$TAMPERED" commit -qam "simulated corruption"

echo "== 3. Writing configuration fixtures (authors, rules, orchestration)"

CFG="$WORK/cfg"
mkdir -p "$CFG"

echo "$SVN_AUTHOR = Real Name <$SVN_AUTHOR@example.com>" >"$CFG/authors.txt"
: >"$CFG/authors-empty.txt"

cat >"$CFG/full.rules" <<'EOF'
create repository myproject
end repository

match /trunk/
  repository myproject
  branch master
end match

match /branches/([^/]+)/
  repository myproject
  branch \1
end match

match /tags/([^/]+)/
  repository myproject
  branch tags/\1
end match
EOF

cat >"$CFG/trunk-only.rules" <<'EOF'
create repository myproject
end repository

match /trunk/
  repository myproject
  branch master
end match
EOF

printf 'migration:\n  name: demo\n  steps:\n    - validate\n    - convert\n' >"$CFG/orchestration.yaml"
printf 'migration:\n\tname: broken by a tab\n' >"$CFG/orchestration-bad.yaml"

echo "== 4. Exercising every command"

# --- usage / help ---------------------------------------------------------
run help 0                 $WRAPPER "$BIN" --help
run missing-svn-url 2      $WRAPPER "$BIN" --dry-run
run unknown-option 2       $WRAPPER "$BIN" --frobnicate "$URL"
run bad-content-samples 2  $WRAPPER "$BIN" --verify-content --content-samples 10abc --git-repo "$MIRROR" "$URL"

# --- pre-flight configuration (--dry-run, --orchestration) ----------------
run dry-run-all-configs 0  $WRAPPER "$BIN" --dry-run --authors "$CFG/authors.txt" \
                               --rules "$CFG/full.rules" \
                               --orchestration "$CFG/orchestration.yaml" "$URL"
run dry-run-bad-yaml 1     $WRAPPER "$BIN" --dry-run --authors "$CFG/authors.txt" \
                               --rules "$CFG/full.rules" \
                               --orchestration "$CFG/orchestration-bad.yaml" "$URL"

# --- authors ---------------------------------------------------------------
run authors-covered 0      $WRAPPER "$BIN" --validate-authors-only --authors "$CFG/authors.txt" "$URL"
run authors-uncovered 1    $WRAPPER "$BIN" --validate-authors-only --authors "$CFG/authors-empty.txt" "$URL"
run auto-map-authors 0     $WRAPPER "$BIN" --auto-map-authors --authors "$CFG/authors-empty.txt" "$URL"
expect_file "$LAST_DIR/authors-generated.txt"

# --- rules coverage (pre-migration cheap-copy protection) ------------------
run rules-coverage-full 0  $WRAPPER "$BIN" --validate-rules-coverage --rules "$CFG/full.rules" "$URL"
run rules-coverage-gap 1   $WRAPPER "$BIN" --validate-rules-coverage --rules "$CFG/trunk-only.rules" "$URL"
expect_in_output "release-1.0"

# --- interactive rule debugger ---------------------------------------------
run debug-rules 0          bash -c "printf '/trunk/\n\n' | $WRAPPER '$BIN' --debug-rules --rules '$CFG/full.rules'"
expect_in_output "myproject"

# --- post-migration content verification -----------------------------------
run verify-standard-layout 0   $WRAPPER "$BIN" --verify-content --git-repo "$MIRROR" "$URL"
expect_file "$LAST_DIR/content_validation_report.txt"
run verify-with-rules 0        $WRAPPER "$BIN" --verify-content --rules "$CFG/full.rules" --git-repo "$MIRROR" "$URL"
run verify-sampled 0           $WRAPPER "$BIN" --verify-content --content-samples 1 --git-repo "$MIRROR" "$URL"
run verify-detects-loss 1      $WRAPPER "$BIN" --verify-content --git-repo "$LOSSY" "$URL"
expect_in_output "missing in git"
expect_in_output "manual.txt"
run verify-detects-tamper 1    $WRAPPER "$BIN" --verify-content --git-repo "$TAMPERED" "$URL"
expect_in_output "content mismatch"
expect_in_output "main.c"

# --- traceability artifacts -------------------------------------------------
run traceability 0         $WRAPPER "$BIN" --generate-traceability-map --git-repo "$MIRROR" "$URL"
expect_file "$LAST_DIR/svn_to_git_mapping.json"
expect_file "$LAST_DIR/traceability.db"
expect_file "$LAST_DIR/audit.log"

if command -v python3 >/dev/null; then
    if python3 - "$LAST_DIR/traceability.db" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = db.execute("SELECT svn_revision, git_commit_sha FROM mappings ORDER BY svn_revision").fetchall()
assert len(rows) == 3, f"expected 3 mappings, got {len(rows)}"
assert [r[0] for r in rows] == [1, 2, 4], f"unexpected revisions: {rows}"
assert all(len(r[1]) == 40 for r in rows), "truncated SHA in database"
print("traceability.db: 3 mappings, revisions 1/2/4, full SHAs")
EOF
    then
        PASS=$((PASS + 1)); echo "PASS  traceability.db is queryable and correct"
    else
        FAIL=$((FAIL + 1)); echo "FAIL  traceability.db query failed"
    fi
else
    echo "SKIP  traceability.db query (python3 not available)"
fi

echo
echo "== RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
