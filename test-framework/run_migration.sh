#!/bin/bash
# Run SVN2Git Migration with Validation

set -e

# Source configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.env"

# Get test session directory from environment or use latest
if [ -z "${TEST_SESSION_DIR}" ]; then
	TEST_SESSION_DIR="${LOG_DIR}/test_$(ls -t "${LOG_DIR}" | grep '^test_' | head -1)"
fi

# Create test session directory if it doesn't exist
mkdir -p "${TEST_SESSION_DIR}"

LOG_FILE="${TEST_SESSION_DIR}/migration.log"

log() {
	echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" | tee -a "${LOG_FILE}"
}

error() {
	echo "[ERROR] $*" | tee -a "${LOG_FILE}" >&2
	return 1
}

log "Starting SVN2Git migration..."
log "SVN Repository: ${SVN_REPO_PATH}"
log "Output Repository: ${GIT_REPO_PATH}"
log "Rules File: ${RULES_FILE}"
log "Authors File: ${AUTHORS_FILE}"

# Check prerequisites
if [ ! -d "${SVN_REPO_PATH}" ]; then
	error "SVN repository not found at ${SVN_REPO_PATH}" && exit 1
fi

if [ ! -f "${SCRIPT_DIR}/${AUTHORS_FILE}" ]; then
	error "Authors file not found: ${SCRIPT_DIR}/${AUTHORS_FILE}" && exit 1
fi

if [ ! -f "${SCRIPT_DIR}/${RULES_FILE}" ]; then
	error "Rules file not found: ${SCRIPT_DIR}/${RULES_FILE}" && exit 1
fi

log "✓ All prerequisites found"

# Check if svn2git binary exists
if [ -z "${SVN2GIT_BINARY}" ] || [ ! -x "${SVN2GIT_BINARY}" ]; then
	log "svn2git binary not found in PATH, attempting to build..."

	# Try to build svn2git from source
	PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
	if [ -d "${PROJECT_ROOT}" ] && [ -f "${PROJECT_ROOT}/CMakeLists.txt" ]; then
		log "Building svn2git from source..."
		BUILD_DIR="${PROJECT_ROOT}/build_test"
		mkdir -p "${BUILD_DIR}"
		cd "${BUILD_DIR}"

		if cmake .. >> "${LOG_FILE}" 2>&1 && make >> "${LOG_FILE}" 2>&1; then
			SVN2GIT_BINARY="${BUILD_DIR}/svn2git/svn2git"
			if [ -x "${SVN2GIT_BINARY}" ]; then
				log "✓ Successfully built svn2git"
			else
				error "Failed to locate built svn2git binary" && exit 1
			fi
		else
			error "Failed to build svn2git" && exit 1
		fi
	else
		error "Could not locate svn2git binary and cannot build from source" && exit 1
	fi
else
	log "Using svn2git: ${SVN2GIT_BINARY}"
fi

# Create output directory
mkdir -p "${GIT_OUTPUT_DIR}"

# Remove existing Git repository if it exists
if [ -d "${GIT_REPO_PATH}" ]; then
	log "Removing previous Git repository..."
	rm -rf "${GIT_REPO_PATH}"
fi

log "Validating authors.txt..."
AUTHOR_COUNT=$(wc -l < "${SCRIPT_DIR}/${AUTHORS_FILE}" | tr -d ' ')
if [ "${AUTHOR_COUNT}" -lt 40 ]; then
	error "Authors file has only ${AUTHOR_COUNT} entries, expected at least 40" && exit 1
fi
log "✓ Authors file validated (${AUTHOR_COUNT} entries)"

log "Validating .rules file..."
if ! grep -q "create repository" "${SCRIPT_DIR}/${RULES_FILE}"; then
	error "Rules file is missing 'create repository' statement" && exit 1
fi
if ! grep -q "match /trunk" "${SCRIPT_DIR}/${RULES_FILE}"; then
	error "Rules file is missing trunk match rule" && exit 1
fi
log "✓ Rules file validated"

# Run svn2git
log "Running svn2git migration..."
MIGRATION_START=$(date +%s)

# Change to output directory for Git repository creation
mkdir -p "${GIT_OUTPUT_DIR}"
cd "${GIT_OUTPUT_DIR}"

log "Command: ${SVN2GIT_BINARY} --identity-map ${SCRIPT_DIR}/${AUTHORS_FILE} --rules ${SCRIPT_DIR}/${RULES_FILE} file://${SVN_REPO_PATH}"

if "${SVN2GIT_BINARY}" \
	--identity-map "${SCRIPT_DIR}/${AUTHORS_FILE}" \
	--rules "${SCRIPT_DIR}/${RULES_FILE}" \
	"file://${SVN_REPO_PATH}" \
	>> "${LOG_FILE}" 2>&1; then
	MIGRATION_END=$(date +%s)
	MIGRATION_DURATION=$((MIGRATION_END - MIGRATION_START))
	log "✓ Migration completed successfully (${MIGRATION_DURATION}s)"
else
	error "Migration failed!" && exit 1
fi

# Verify Git repository was created (svn2git creates it in current directory based on .rules)
ACTUAL_GIT_REPO_PATH="${GIT_OUTPUT_DIR}/platform-git"
if [ ! -d "${ACTUAL_GIT_REPO_PATH}" ]; then
	error "Git repository not created at ${ACTUAL_GIT_REPO_PATH}" && exit 1
fi
log "✓ Git repository created at ${ACTUAL_GIT_REPO_PATH}"

# Export migration information
cat > "${TEST_SESSION_DIR}/migration_info.txt" << EOF
Migration Execution Report
==========================
Start Time: $(date -d @${MIGRATION_START})
End Time: $(date -d @${MIGRATION_END})
Duration (seconds): ${MIGRATION_DURATION}

SVN Repository: ${SVN_REPO_PATH}
Git Repository: ${ACTUAL_GIT_REPO_PATH}
Rules File: ${SCRIPT_DIR}/${RULES_FILE}
Authors File: ${SCRIPT_DIR}/${AUTHORS_FILE}

svn2git Binary: ${SVN2GIT_BINARY}

Status: SUCCESS
EOF

log "✓ Migration info saved"
log "✓ SVN2Git migration complete!"

exit 0
