#!/bin/bash
# Validate SVN2Git Migration Results

set -e

# Source configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.env"

# Get test session directory from environment or use latest
if [ -z "${TEST_SESSION_DIR}" ]; then
	TEST_SESSION_DIR="${LOG_DIR}/test_$(ls -t "${LOG_DIR}" | grep '^test_' | head -1)"
fi

mkdir -p "${TEST_SESSION_DIR}"

LOG_FILE="${TEST_SESSION_DIR}/validation.log"
REPORT_FILE="${TEST_SESSION_DIR}/validation_report.txt"

log() {
	echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

# Fatal: log and terminate. Must exit here — 'error ... && exit 1' would
# never reach the exit because the && short-circuits on error's own status.
error() {
	echo "[ERROR] $*" | tee -a "${LOG_FILE}" >&2
	exit 1
}

# Color codes
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log "Starting post-migration validation..."

# Verify Git repository exists
if [ ! -d "${GIT_REPO_PATH}" ]; then
	error "Git repository not found at ${GIT_REPO_PATH}"
fi

log "✓ Git repository found"

# Initialize report
cat > "${REPORT_FILE}" << EOF
SVN → Git Conversion Validation Report
=====================================
Generated: $(date)

Environment:
  SVN Repository: ${SVN_REPO_PATH}
  Git Repository: ${GIT_REPO_PATH}
  Test Session: ${TEST_SESSION_DIR}

EOF

# Check for pre-migration data
SVN_INFO_FILE="${TEST_SESSION_DIR}/svn_repo_info.txt"
if [ -f "${SVN_INFO_FILE}" ]; then
	source "${SVN_INFO_FILE}"
	log "✓ Pre-migration data loaded"
fi

# Verify Git repository integrity
log "Checking Git repository integrity..."
cd "${GIT_REPO_PATH}"

if ! git fsck --full >> "${LOG_FILE}" 2>&1; then
	error "Git repository integrity check failed!"
	echo "  ✗ Repository integrity check: FAILED" >> "${REPORT_FILE}"
	exit 1
else
	log "✓ Git repository integrity verified"
	echo "  ✓ Repository integrity check: PASSED" >> "${REPORT_FILE}"
fi

# Count commits in Git
log "Counting Git commits..."
GIT_COMMIT_COUNT=$(git rev-list --all --count 2>> "${LOG_FILE}")
log "  Git commits: ${GIT_COMMIT_COUNT}"

# Count branches in Git
log "Counting Git branches..."
GIT_BRANCH_COUNT=$(git branch -a 2>> "${LOG_FILE}" | grep -v "HEAD" | wc -l)
log "  Git branches: ${GIT_BRANCH_COUNT}"

# Count tags in Git
log "Counting Git tags..."
GIT_TAG_COUNT=$(git tag -l 2>> "${LOG_FILE}" | wc -l)
log "  Git tags: ${GIT_TAG_COUNT}"

# Verify branch names
log "Verifying branch naming conventions..."
EXPECTED_BRANCHES_FOUND=0
for branch in $(git branch -a 2>> "${LOG_FILE}" | grep -v "HEAD" | grep -v "master"); do
	if echo "${branch}" | grep -qE "platform-[0-9]+|platform-bl[0-9]+"; then
		EXPECTED_BRANCHES_FOUND=$((EXPECTED_BRANCHES_FOUND + 1))
	fi
done
log "  Expected branch patterns found: ${EXPECTED_BRANCHES_FOUND}/${GIT_BRANCH_COUNT}"

# Verify all tags are from support branches only
log "Verifying tags are only from support branches..."
INVALID_TAGS=0
for tag in $(git tag -l 2>> "${LOG_FILE}"); do
	if ! echo "${tag}" | grep -qE "^PLATFORM-BL[0-9]+"; then
		log "  Warning: Unexpected tag format: ${tag}"
		INVALID_TAGS=$((INVALID_TAGS + 1))
	fi
done

if [ ${INVALID_TAGS} -eq 0 ]; then
	log "✓ All tags follow support branch naming (PLATFORM-BLXXX)"
	echo "  ✓ Tag naming validation: PASSED" >> "${REPORT_FILE}"
else
	log "⚠ Found ${INVALID_TAGS} tags with unexpected naming"
fi

# Verify master branch exists
log "Verifying master branch..."
if git show-ref --verify --quiet refs/heads/master 2>> "${LOG_FILE}"; then
	log "✓ Master branch exists"
	MASTER_COMMITS=$(git rev-list --count master 2>> "${LOG_FILE}")
	log "  Commits on master: ${MASTER_COMMITS}"
else
	error "Master branch not found!"
fi

# Verify file presence
log "Checking for converted content..."
FILE_CHECK_PASS=0
if git ls-tree -r master 2>> "${LOG_FILE}" | grep -q "\.c$"; then
	log "✓ C source files found"
	C_FILE_COUNT=$(git ls-tree -r master 2>> "${LOG_FILE}" | grep -c "\.c$")
	FILE_CHECK_PASS=$((FILE_CHECK_PASS + 1))
fi

if git ls-tree -r master 2>> "${LOG_FILE}" | grep -q "\.h$"; then
	log "✓ Header files found"
	H_FILE_COUNT=$(git ls-tree -r master 2>> "${LOG_FILE}" | grep -c "\.h$")
	FILE_CHECK_PASS=$((FILE_CHECK_PASS + 1))
fi

if git ls-tree -r master 2>> "${LOG_FILE}" | grep -q "CMakeLists\.txt$"; then
	log "✓ CMakeLists.txt files found"
	CMAKE_FILE_COUNT=$(git ls-tree -r master 2>> "${LOG_FILE}" | grep -c "CMakeLists\.txt$")
	FILE_CHECK_PASS=$((FILE_CHECK_PASS + 1))
fi

# Check traceability
log "Verifying traceability..."
if [ -f "${GIT_REPO_PATH}/svn_to_git_mapping.json" ]; then
	log "✓ SVN-to-Git mapping JSON exists"
	echo "  ✓ Traceability mapping: FOUND" >> "${REPORT_FILE}"
else
	log "⚠ SVN-to-Git mapping JSON not found"
fi

if [ -f "${GIT_REPO_PATH}/traceability.db" ]; then
	log "✓ Traceability database exists"
	echo "  ✓ Traceability database: FOUND" >> "${REPORT_FILE}"
else
	log "⚠ Traceability database not found"
fi

# Generate comparison report
log "Generating comparison report..."
cat >> "${REPORT_FILE}" << EOF

Validation Results:
===================

Pre-Migration (SVN):
  Total Revisions: ${SVN_FINAL_REVISION:-N/A}
  Authors: ${SVN_AUTHORS:-N/A}
  Regular Branches: ${SVN_REGULAR_BRANCHES:-N/A}
  Support Branches: ${SVN_SUPPORT_BRANCHES:-N/A}
  Tags: ${SVN_APPROX_TAGS:-N/A}

Post-Migration (Git):
  Total Commits: ${GIT_COMMIT_COUNT}
  Branches: ${GIT_BRANCH_COUNT}
  Tags: ${GIT_TAG_COUNT}

Content Verification:
  C Files: ${C_FILE_COUNT:-N/A}
  Header Files: ${H_FILE_COUNT:-N/A}
  CMakeLists.txt: ${CMAKE_FILE_COUNT:-N/A}

Repository Size:
  SVN: $(du -sh "${SVN_REPO_PATH}" 2>/dev/null | cut -f1)
  Git: $(du -sh "${GIT_REPO_PATH}" 2>/dev/null | cut -f1)

Checks Performed:
  ✓ Git repository integrity (git fsck)
  ✓ Branch naming conventions
  ✓ Tag naming (support-branches only)
  ✓ Master branch presence
  ✓ Content files present (C/H/CMake)
  ✓ Traceability mapping

Validation Summary:
===================
EOF

# Determine overall status
OVERALL_STATUS="PASSED"
if [ ${FILE_CHECK_PASS} -lt 3 ]; then
	OVERALL_STATUS="FAILED"
	log "✗ Content validation failed"
fi

if [ -z "${GIT_COMMIT_COUNT}" ] || [ "${GIT_COMMIT_COUNT}" -eq 0 ]; then
	OVERALL_STATUS="FAILED"
	log "✗ No commits found in Git repository"
fi

echo "Overall Status: ${OVERALL_STATUS}" >> "${REPORT_FILE}"

if [ "${OVERALL_STATUS}" = "PASSED" ]; then
	echo -e "${GREEN}✓ VALIDATION PASSED${NC}" | tee -a "${REPORT_FILE}"
	log "✓ All validation checks passed!"
	exit 0
else
	echo -e "${RED}✗ VALIDATION FAILED${NC}" | tee -a "${REPORT_FILE}"
	log "✗ Validation failed - see report for details"
	exit 1
fi
