#!/bin/bash
# Exercise every svn2git-validate CLI option end-to-end.
#
# svn2git-validate cannot combine all flags in one invocation:
#   --debug-rules returns immediately after the interactive session, and
#   --validate-authors-only exits right after the author coverage check.
# So the full option surface is covered across sequential phases, each in
# its own working directory (the tool writes audit.log, error_report.txt,
# authors-generated.txt, svn_to_git_mapping.json and traceability.db to
# the current directory with fixed names).
#
# Flags covered: --help --debug-rules --rules --validate-authors-only
#   --authors --auto-map-authors --dry-run --debug --orchestration
#   --log-file --operator --generate-traceability-map --git-repo
#   --push-gitlab  (+ usage-error exit-code paths)
#
# Usage: ./test_validate_options.sh
#   WORK_DIR=/path ./test_validate_options.sh   # custom workspace

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/svn2git_validate_test}"
VALIDATE="${REPO_ROOT}/build/svn2git-validate"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0
FAILED_PHASES=""

print_header() { echo -e "${BLUE}==> $1${NC}"; }
print_pass() { echo -e "${GREEN}✓ PASS${NC} $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
print_fail() {
	echo -e "${RED}✗ FAIL${NC} $1"
	FAIL_COUNT=$((FAIL_COUNT + 1))
	FAILED_PHASES="${FAILED_PHASES} $1"
}

# run_phase <phase-id> <expected-exit-code> <workdir> <stdin-file> <cmd...>
# Runs cmd in workdir with stdin from stdin-file, captures output.log,
# and records pass/fail on the exit code. Extra artifact checks are done
# by the caller with check_* helpers.
PHASE_RC=0
run_phase() {
	local phase="$1" expected_rc="$2" dir="$3" stdin_src="$4"
	shift 4
	mkdir -p "${dir}"
	PHASE_RC=0
	(cd "${dir}" && "$@" <"${stdin_src}") >"${dir}/output.log" 2>&1 || PHASE_RC=$?
	if [ "${PHASE_RC}" -eq "${expected_rc}" ]; then
		print_pass "${phase}: exit ${PHASE_RC} (expected ${expected_rc})"
	else
		print_fail "${phase}: exit ${PHASE_RC} (expected ${expected_rc})"
		sed 's/^/    /' "${dir}/output.log" | tail -10
	fi
}

check_file() {
	local phase="$1" file="$2"
	if [ -s "${file}" ]; then
		print_pass "${phase}: $(basename "${file}") exists"
	else
		print_fail "${phase}: $(basename "${file}") missing or empty"
	fi
}

check_grep() {
	local phase="$1" pattern="$2" file="$3"
	if grep -qi -- "${pattern}" "${file}" 2>/dev/null; then
		print_pass "${phase}: '${pattern}' found in $(basename "${file}")"
	else
		print_fail "${phase}: '${pattern}' not found in $(basename "${file}")"
	fi
}

# ---------------------------------------------------------------------------
print_header "Prerequisites"

for tool in git svn svnadmin cmake sqlite3 python3; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		echo -e "${RED}Missing required tool: ${tool}${NC}"
		echo "  Install with: apt-get install -y subversion sqlite3 cmake git python3"
		exit 1
	fi
done

if [ ! -x "${VALIDATE}" ]; then
	print_header "Building svn2git-validate"
	cmake -B "${REPO_ROOT}/build" -S "${REPO_ROOT}" -DCMAKE_BUILD_TYPE=Release || exit 1
	cmake --build "${REPO_ROOT}/build" -j"$(nproc)" --target svn2git-validate || exit 1
fi
echo "Binary: ${VALIDATE}"

# ---------------------------------------------------------------------------
print_header "Preparing fixtures in ${WORK_DIR}"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

# Small SVN repository: 6 revisions, 2 authors (jsmith, mmustermann)
if ! sh "${REPO_ROOT}/tests/fixtures/create-test-svn-repo.sh" "${WORK_DIR}/svn" >/dev/null; then
	echo -e "${RED}Failed to create SVN fixture repository${NC}"
	exit 1
fi
SVN_URL="file://${WORK_DIR}/svn/repo"
echo "SVN fixture: ${SVN_URL}"

# Complete and deliberately-incomplete author mappings
cat >"${WORK_DIR}/authors.txt" <<'EOF'
jsmith = John Smith <john.smith@example.com>
mmustermann = Max Mustermann <max.mustermann@example.com>
EOF
cat >"${WORK_DIR}/authors-incomplete.txt" <<'EOF'
jsmith = John Smith <john.smith@example.com>
EOF

cp "${REPO_ROOT}/tests/fixtures/test.rules" "${WORK_DIR}/test.rules"

cat >"${WORK_DIR}/orchestration.yml" <<'EOF'
---
migration:
  repository: testproject
  steps:
    - validate
    - convert
EOF

# A "converted" git repository with git-svn-id trailers so the traceability
# map uses real SVN revision metadata instead of positional numbering.
GIT_REPO="${WORK_DIR}/converted-git"
git init -q -b main "${GIT_REPO}"
git -C "${GIT_REPO}" config user.name "Test Engineer"
git -C "${GIT_REPO}" config user.email "testengr@example.com"
SVN_UUID="00000000-0000-0000-0000-000000000000"
for rev in 1 2 3 4 5 6; do
	echo "content at r${rev}" >"${GIT_REPO}/file.txt"
	git -C "${GIT_REPO}" add file.txt
	git -C "${GIT_REPO}" commit -q -m "Converted commit r${rev}

git-svn-id: ${SVN_URL}/trunk@${rev} ${SVN_UUID}"
done
git -C "${GIT_REPO}" branch release-1.0
git -C "${GIT_REPO}" tag v1.0.0
echo "Git fixture: ${GIT_REPO} (6 commits, 1 branch, 1 tag)"

# Local bare repository standing in for a GitLab remote
FAKE_GITLAB="${WORK_DIR}/fake-gitlab.git"
git init -q --bare "${FAKE_GITLAB}"
echo "Fake GitLab remote: ${FAKE_GITLAB}"

# ---------------------------------------------------------------------------
print_header "Phase 1: --help"
run_phase "phase1" 0 "${WORK_DIR}/phase1" /dev/null \
	"${VALIDATE}" --help
check_grep "phase1" "usage:" "${WORK_DIR}/phase1/output.log"

# ---------------------------------------------------------------------------
print_header "Phase 2: --debug-rules + --rules (stdin-fed, no hang)"
printf '/trunk/src/main.c\n\n' >"${WORK_DIR}/phase2-stdin.txt"
run_phase "phase2" 0 "${WORK_DIR}/phase2" "${WORK_DIR}/phase2-stdin.txt" \
	"${VALIDATE}" --debug-rules --rules "${WORK_DIR}/test.rules"
check_grep "phase2" "trunk" "${WORK_DIR}/phase2/output.log"

# ---------------------------------------------------------------------------
print_header "Phase 3: --validate-authors-only + --authors (full coverage)"
run_phase "phase3" 0 "${WORK_DIR}/phase3" /dev/null \
	"${VALIDATE}" --validate-authors-only --authors "${WORK_DIR}/authors.txt" "${SVN_URL}"
check_file "phase3" "${WORK_DIR}/phase3/audit.log"

# ---------------------------------------------------------------------------
print_header "Phase 3b: --validate-authors-only (missing author → exit 1)"
run_phase "phase3b" 1 "${WORK_DIR}/phase3b" /dev/null \
	"${VALIDATE}" --validate-authors-only --authors "${WORK_DIR}/authors-incomplete.txt" "${SVN_URL}"
check_file "phase3b" "${WORK_DIR}/phase3b/error_report.txt"
check_grep "phase3b" "E002_MISSING_AUTHORS" "${WORK_DIR}/phase3b/error_report.txt"
check_grep "phase3b" "mmustermann" "${WORK_DIR}/phase3b/output.log"

# ---------------------------------------------------------------------------
print_header "Phase 4: --auto-map-authors (generates placeholders)"
run_phase "phase4" 0 "${WORK_DIR}/phase4" /dev/null \
	"${VALIDATE}" --auto-map-authors --authors "${WORK_DIR}/authors-incomplete.txt" "${SVN_URL}"
check_file "phase4" "${WORK_DIR}/phase4/authors-generated.txt"
check_grep "phase4" "mmustermann" "${WORK_DIR}/phase4/authors-generated.txt"

# ---------------------------------------------------------------------------
print_header "Phase 5: --dry-run --debug --orchestration --log-file --operator"
run_phase "phase5" 0 "${WORK_DIR}/phase5" /dev/null \
	"${VALIDATE}" --dry-run --debug \
	--authors "${WORK_DIR}/authors.txt" \
	--rules "${WORK_DIR}/test.rules" \
	--orchestration "${WORK_DIR}/orchestration.yml" \
	--log-file custom.log \
	--operator testengr \
	"${SVN_URL}"
check_file "phase5" "${WORK_DIR}/phase5/custom.log"
check_file "phase5" "${WORK_DIR}/phase5/audit.log"
check_grep "phase5" "testengr" "${WORK_DIR}/phase5/audit.log"

# ---------------------------------------------------------------------------
print_header "Phase 6: --generate-traceability-map --git-repo --push-gitlab --operator"
run_phase "phase6" 0 "${WORK_DIR}/phase6" /dev/null \
	"${VALIDATE}" --generate-traceability-map \
	--git-repo "${GIT_REPO}" \
	--push-gitlab "${FAKE_GITLAB}" \
	--operator testengr
check_file "phase6" "${WORK_DIR}/phase6/svn_to_git_mapping.json"
check_file "phase6" "${WORK_DIR}/phase6/traceability.db"

# JSON is valid and carries all 6 revisions
if python3 -c "
import json, sys
with open('${WORK_DIR}/phase6/svn_to_git_mapping.json') as f:
    data = json.load(f)
sys.exit(0 if data.get('total_mappings') == 6 else 1)
" 2>/dev/null; then
	print_pass "phase6: mapping JSON valid with total_mappings=6"
else
	print_fail "phase6: mapping JSON invalid or wrong total_mappings"
fi

# SQLite database is queryable with one row per commit
DB_COUNT=$(sqlite3 "${WORK_DIR}/phase6/traceability.db" 'SELECT COUNT(*) FROM mappings;' 2>/dev/null || echo "error")
if [ "${DB_COUNT}" = "6" ]; then
	print_pass "phase6: traceability.db has 6 mappings"
else
	print_fail "phase6: traceability.db mappings count = ${DB_COUNT} (expected 6)"
fi

# All refs and tags arrived at the fake GitLab remote
REMOTE_REFS=$(git -C "${FAKE_GITLAB}" for-each-ref --format='%(refname)' | sort | tr '\n' ' ')
if echo "${REMOTE_REFS}" | grep -q "refs/heads/main" \
	&& echo "${REMOTE_REFS}" | grep -q "refs/heads/release-1.0" \
	&& echo "${REMOTE_REFS}" | grep -q "refs/tags/v1.0.0"; then
	print_pass "phase6: remote has main, release-1.0 and v1.0.0"
else
	print_fail "phase6: remote refs incomplete: ${REMOTE_REFS}"
fi

# ---------------------------------------------------------------------------
print_header "Phase 7: --generate-traceability-map without --git-repo (usage error)"
run_phase "phase7" 2 "${WORK_DIR}/phase7" /dev/null \
	"${VALIDATE}" --generate-traceability-map

# ---------------------------------------------------------------------------
print_header "Phase 8: unknown option (usage error)"
run_phase "phase8" 2 "${WORK_DIR}/phase8" /dev/null \
	"${VALIDATE}" --bogus-flag x

# ---------------------------------------------------------------------------
echo ""
print_header "Summary"
echo "  Flags covered: --help --debug-rules --rules --validate-authors-only"
echo "                 --authors --auto-map-authors --dry-run --debug"
echo "                 --orchestration --log-file --operator"
echo "                 --generate-traceability-map --git-repo --push-gitlab"
echo ""
echo -e "  Checks passed: ${GREEN}${PASS_COUNT}${NC}"
echo -e "  Checks failed: ${RED}${FAIL_COUNT}${NC}"
echo "  Workspace: ${WORK_DIR}"

if [ "${FAIL_COUNT}" -eq 0 ]; then
	echo -e "${GREEN}✓ ALL svn2git-validate OPTION TESTS PASSED${NC}"
	exit 0
else
	echo -e "${RED}✗ FAILURES:${FAILED_PHASES}${NC}"
	exit 1
fi
