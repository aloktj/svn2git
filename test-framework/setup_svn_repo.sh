#!/bin/bash
# Setup SVN Repository with 10,000+ revisions and 61 branches (51 feature + 10 support)

set -e

# Source configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.env"

# Create logging directory
mkdir -p "${TEST_SESSION_DIR}"
LOG_FILE="${TEST_SESSION_DIR}/svn_setup.log"

log() {
	echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

error() {
	echo "[ERROR] $*" | tee -a "${LOG_FILE}" >&2
	exit 1
}

log "Starting SVN repository setup..."
log "SVN Repo Path: ${SVN_REPO_PATH}"
log "Total Revisions Target: ${TOTAL_REVISIONS}"
log "Total Branches: ${TOTAL_BRANCHES}"
log "Total Authors: ${NUM_AUTHORS}"

# Check prerequisites
if [ ! -x "${SVNADMIN_PATH}" ]; then
	error "svnadmin not found at ${SVNADMIN_PATH}"
fi

log "Using svnadmin: ${SVNADMIN_PATH}"

# Create SVN repository directory
mkdir -p "${SVN_ROOT_DIR}"
if [ -d "${SVN_REPO_PATH}" ]; then
	error "SVN repository already exists at ${SVN_REPO_PATH}. Please remove it first."
fi

log "Creating SVN repository..."
"${SVNADMIN_PATH}" create "${SVN_REPO_PATH}" >> "${LOG_FILE}" 2>&1
log "✓ SVN repository created"

# Enable pre-revprop-change hook to allow revision property changes
HOOKS_DIR="${SVN_REPO_PATH}/hooks"
PRE_REVPROP_HOOK="${HOOKS_DIR}/pre-revprop-change"
cat > "${PRE_REVPROP_HOOK}" << 'EOF'
#!/bin/bash
exit 0
EOF
chmod +x "${PRE_REVPROP_HOOK}"
log "✓ Configured pre-revprop-change hook"

# Create temporary working directory
WORK_DIR=$(mktemp -d)
trap "rm -rf ${WORK_DIR}" EXIT

log "Working directory: ${WORK_DIR}"

# Check out the repository
REPO_WC="${WORK_DIR}/repo_wc"
mkdir -p "${REPO_WC}"
"${SVN_PATH}" checkout "file://${SVN_REPO_PATH}" "${REPO_WC}" >> "${LOG_FILE}" 2>&1
log "✓ Repository checked out"

# Create standard SVN structure: trunk, branches, tags
log "Creating standard SVN layout (trunk/branches/tags)..."
mkdir -p "${REPO_WC}/trunk"
mkdir -p "${REPO_WC}/branches"
mkdir -p "${REPO_WC}/tags"

# Generate test data in trunk
log "Generating test data..."
"${SCRIPT_DIR}/generate_test_data.sh" "${NUM_C_FILES}" "${NUM_H_FILES}" "${REPO_WC}/trunk" >> "${LOG_FILE}" 2>&1
log "✓ Test data generated (${NUM_C_FILES} .c files, ${NUM_H_FILES} .h files, CMakeLists.txt)"

# Initial commit with directory structure
cd "${REPO_WC}"
"${SVN_PATH}" add trunk branches tags >> "${LOG_FILE}" 2>&1
"${SVN_PATH}" commit -m "Initial commit: Create standard SVN layout" --username "alice" >> "${LOG_FILE}" 2>&1
log "✓ Initial commit (revision 1)"

# Generate commits on trunk
log "Generating ${TRUNK_COMMITS} commits on trunk..."
TRUNK_COMMIT_COUNT=0
COMMIT_INCREMENT=$((TRUNK_COMMITS / 100))  # Progress indicator every 100 commits
COMMIT_INTERVAL=$((TRUNK_COMMITS / 50))    # 50 progress checkpoints

for ((i = 1; i <= TRUNK_COMMITS; i++)); do
	# Random author from our 50-author pool
	AUTHOR_INDEX=$((RANDOM % NUM_AUTHORS))
	AUTHOR=$(sed -n "$((AUTHOR_INDEX + 1))p" "${SCRIPT_DIR}/authors.txt" | cut -d'=' -f1 | xargs)

	# Randomly modify a file
	FILE_NUM=$((RANDOM % NUM_C_FILES + 1))
	FILE_TO_MODIFY="trunk/src/module_${FILE_NUM}.c"

	# Append a comment with timestamp
	echo "/* Updated at commit $i */" >> "${REPO_WC}/${FILE_TO_MODIFY}"

	# Create meaningful commit message
	MESSAGES=(
		"Update module_${FILE_NUM} with improvements"
		"Fix bug in module_${FILE_NUM} processing"
		"Refactor module_${FILE_NUM} for clarity"
		"Add feature to module_${FILE_NUM}"
		"Optimize module_${FILE_NUM} performance"
		"Add comments to module_${FILE_NUM}"
	)
	MESSAGE=${MESSAGES[$((RANDOM % ${#MESSAGES[@]}))]}" (revision $i)"

	"${SVN_PATH}" commit -m "${MESSAGE}" --username "${AUTHOR}" >> "${LOG_FILE}" 2>&1

	((TRUNK_COMMIT_COUNT++))

	# Progress indicator
	if [ $((i % COMMIT_INTERVAL)) -eq 0 ]; then
		PROGRESS=$((i * 100 / TRUNK_COMMITS))
		log "  Trunk commits: ${PROGRESS}% (${i}/${TRUNK_COMMITS})"
	fi
done
log "✓ Generated ${TRUNK_COMMIT_COUNT} commits on trunk"

# Create branches
log "Creating ${NUM_REGULAR_BRANCHES} feature branches..."
CURRENT_REVISION=$("${SVN_PATH}" info "${REPO_WC}/trunk" | grep "Revision:" | awk '{print $2}')

for ((i = 0; i < NUM_REGULAR_BRANCHES; i++)); do
	BRANCH_NAME="platform-$((1000 + i))"
	BRANCH_PATH="branches/${BRANCH_NAME}"

	"${SVN_PATH}" copy trunk "${BRANCH_PATH}" -m "Create branch ${BRANCH_NAME}" --username "alice" >> "${LOG_FILE}" 2>&1

	if [ $((i % 10)) -eq 0 ]; then
		log "  Created $((i + 1))/${NUM_REGULAR_BRANCHES} feature branches"
	fi
done
log "✓ Created ${NUM_REGULAR_BRANCHES} feature branches"

# Create support branches
log "Creating ${NUM_SUPPORT_BRANCHES} support branches..."
for ((i = 2; i <= NUM_SUPPORT_BRANCHES + 1; i++)); do
	BRANCH_NAME="platform-bl10$i"
	BRANCH_PATH="branches/${BRANCH_NAME}"

	"${SVN_PATH}" copy trunk "${BRANCH_PATH}" -m "Create support branch ${BRANCH_NAME}" --username "alice" >> "${LOG_FILE}" 2>&1

	if [ $((i % 5)) -eq 0 ]; then
		log "  Created $((i - 1))/${NUM_SUPPORT_BRANCHES} support branches"
	fi
done
log "✓ Created ${NUM_SUPPORT_BRANCHES} support branches"

# Generate commits on feature branches
log "Generating ${FEATURE_BRANCHES_COMMITS} commits across feature branches..."
FEATURE_COMMIT_COUNT=0

for ((i = 1; i <= FEATURE_BRANCHES_COMMITS; i++)); do
	# Pick a random feature branch
	BRANCH_INDEX=$((RANDOM % NUM_REGULAR_BRANCHES))
	BRANCH_NAME="platform-$((1000 + BRANCH_INDEX))"
	BRANCH_PATH="branches/${BRANCH_NAME}"

	# Check out the branch for modification (in place modification)
	FILE_NUM=$((RANDOM % NUM_C_FILES + 1))
	FILE_TO_MODIFY="${BRANCH_PATH}/src/module_${FILE_NUM}.c"

	echo "/* Feature branch commit at iteration $i */" >> "${REPO_WC}/${FILE_TO_MODIFY}"

	AUTHOR_INDEX=$((RANDOM % NUM_AUTHORS))
	AUTHOR=$(sed -n "$((AUTHOR_INDEX + 1))p" "${SCRIPT_DIR}/authors.txt" | cut -d'=' -f1 | xargs)

	MESSAGES=(
		"Branch feature: Add new capability to module_${FILE_NUM}"
		"Branch fix: Resolve issue in module_${FILE_NUM}"
		"Branch improvement: Enhance module_${FILE_NUM} performance"
		"Branch refactor: Clean up module_${FILE_NUM} code"
	)
	MESSAGE=${MESSAGES[$((RANDOM % ${#MESSAGES[@]}))]}" (branch commit $i)"

	"${SVN_PATH}" commit -m "${MESSAGE}" --username "${AUTHOR}" >> "${LOG_FILE}" 2>&1

	((FEATURE_COMMIT_COUNT++))

	if [ $((i % 500)) -eq 0 ]; then
		PROGRESS=$((i * 100 / FEATURE_BRANCHES_COMMITS))
		log "  Feature branch commits: ${PROGRESS}% (${i}/${FEATURE_BRANCHES_COMMITS})"
	fi
done
log "✓ Generated ${FEATURE_COMMIT_COUNT} commits on feature branches"

# Generate commits on support branches
log "Generating ${SUPPORT_BRANCHES_COMMITS} commits on support branches..."
SUPPORT_COMMIT_COUNT=0

for ((i = 1; i <= SUPPORT_BRANCHES_COMMITS; i++)); do
	# Pick a random support branch
	SUPPORT_INDEX=$((RANDOM % NUM_SUPPORT_BRANCHES + 2))
	BRANCH_NAME="platform-bl10${SUPPORT_INDEX}"
	BRANCH_PATH="branches/${BRANCH_NAME}"

	FILE_NUM=$((RANDOM % NUM_C_FILES + 1))
	FILE_TO_MODIFY="${BRANCH_PATH}/src/module_${FILE_NUM}.c"

	echo "/* Support branch maintenance commit at iteration $i */" >> "${REPO_WC}/${FILE_TO_MODIFY}"

	AUTHOR_INDEX=$((RANDOM % NUM_AUTHORS))
	AUTHOR=$(sed -n "$((AUTHOR_INDEX + 1))p" "${SCRIPT_DIR}/authors.txt" | cut -d'=' -f1 | xargs)

	MESSAGES=(
		"Maintenance: Patch module_${FILE_NUM} for stability"
		"Support: Backport fix to module_${FILE_NUM}"
		"Maintenance: Update version in module_${FILE_NUM}"
		"Support: Security update for module_${FILE_NUM}"
	)
	MESSAGE=${MESSAGES[$((RANDOM % ${#MESSAGES[@]}))]}" (support commit $i)"

	"${SVN_PATH}" commit -m "${MESSAGE}" --username "${AUTHOR}" >> "${LOG_FILE}" 2>&1

	((SUPPORT_COMMIT_COUNT++))

	if [ $((i % 200)) -eq 0 ]; then
		PROGRESS=$((i * 100 / SUPPORT_BRANCHES_COMMITS))
		log "  Support branch commits: ${PROGRESS}% (${i}/${SUPPORT_BRANCHES_COMMITS})"
	fi
done
log "✓ Generated ${SUPPORT_COMMIT_COUNT} commits on support branches"

# Create tags ONLY from support branches
log "Creating tags from support branches..."
TAG_COUNT=0

for ((i = 2; i <= NUM_SUPPORT_BRANCHES + 1; i++)); do
	BRANCH_NAME="platform-bl10${i}"
	BRANCH_PATH="branches/${BRANCH_NAME}"
	TAG_COUNT=$((i - 1))

	# Create multiple tags per support branch
	for ((t = 1; t <= 3; t++)); do
		TAG_NAME="PLATFORM-BL10${i}-v${t}"
		TAG_PATH="tags/${TAG_NAME}"

		"${SVN_PATH}" copy "${BRANCH_PATH}" "${TAG_PATH}" -m "Create tag ${TAG_NAME} from support branch ${BRANCH_NAME}" --username "alice" >> "${LOG_FILE}" 2>&1
	done
done
log "✓ Created tags from support branches (approximately $((NUM_SUPPORT_BRANCHES * 3)) tags)"

# Verify repository
FINAL_REVISION=$("${SVN_PATH}" info "${REPO_WC}" | grep "Revision:" | awk '{print $2}')
log "Final repository revision: ${FINAL_REVISION}"

# Cleanup
cd /
rm -rf "${REPO_WC}"

# Generate pre-migration report
REPORT_FILE="${TEST_SESSION_DIR}/svn_pre_migration_report.txt"
cat > "${REPORT_FILE}" << EOF
SVN Pre-Migration Analysis Report
==================================
Generated: $(date)

Repository Path: ${SVN_REPO_PATH}
Total Revisions: ${FINAL_REVISION}
Trunk Commits: ${TRUNK_COMMIT_COUNT}
Feature Branch Commits: ${FEATURE_COMMIT_COUNT}
Support Branch Commits: ${SUPPORT_COMMIT_COUNT}

Structure:
- Regular Branches: ${NUM_REGULAR_BRANCHES}
- Support Branches: ${NUM_SUPPORT_BRANCHES}
- Tags: ~$((NUM_SUPPORT_BRANCHES * 3))
- Total Authors: ${NUM_AUTHORS}
- C Files: ${NUM_C_FILES}
- Header Files: ${NUM_H_FILES}
- CMakeLists.txt: 2

Repository Size: $(du -sh "${SVN_REPO_PATH}" | cut -f1)

Status: READY FOR MIGRATION
EOF

log "✓ Pre-migration report generated: ${REPORT_FILE}"
log "✓ SVN repository setup complete!"

# Export repository information
cat > "${TEST_SESSION_DIR}/svn_repo_info.txt" << EOF
SVN_REPO_PATH=${SVN_REPO_PATH}
SVN_FINAL_REVISION=${FINAL_REVISION}
SVN_TOTAL_COMMITS=$((FINAL_REVISION - 1))
SVN_AUTHORS=${NUM_AUTHORS}
SVN_REGULAR_BRANCHES=${NUM_REGULAR_BRANCHES}
SVN_SUPPORT_BRANCHES=${NUM_SUPPORT_BRANCHES}
SVN_APPROX_TAGS=$((NUM_SUPPORT_BRANCHES * 3))
EOF

log "Test session directory: ${TEST_SESSION_DIR}"
exit 0
