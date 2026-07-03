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

# Fatal: log and terminate. Must exit here — 'error ... && exit 1' would
# never reach the exit because the && short-circuits on error's own status.
error() {
	echo "[ERROR] $*" | tee -a "${LOG_FILE}" >&2
	exit 1
}

log "Starting SVN2Git migration..."
log "SVN Repository: ${SVN_REPO_PATH}"
log "Output Repository: ${GIT_REPO_PATH}"
log "Rules File: ${RULES_FILE}"
log "Authors File: ${AUTHORS_FILE}"

# Check prerequisites
if [ ! -d "${SVN_REPO_PATH}" ]; then
	error "SVN repository not found at ${SVN_REPO_PATH}"
fi

if [ ! -f "${SCRIPT_DIR}/${AUTHORS_FILE}" ]; then
	error "Authors file not found: ${SCRIPT_DIR}/${AUTHORS_FILE}"
fi

if [ ! -f "${SCRIPT_DIR}/${RULES_FILE}" ]; then
	error "Rules file not found: ${SCRIPT_DIR}/${RULES_FILE}"
fi

log "✓ All prerequisites found"

# Locate the converter binary (svn-all-fast-export, built via qmake && make
# at the repo root — the CMake build only produces svn2git-validate).
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
if [ -z "${SVN2GIT_BINARY}" ] || [ ! -x "${SVN2GIT_BINARY}" ]; then
	if [ -x "${PROJECT_ROOT}/svn-all-fast-export" ]; then
		SVN2GIT_BINARY="${PROJECT_ROOT}/svn-all-fast-export"
	fi
fi

if [ -z "${SVN2GIT_BINARY}" ] || [ ! -x "${SVN2GIT_BINARY}" ]; then
	log "svn-all-fast-export not found, attempting qmake build..."
	if command -v qmake >/dev/null 2>&1 && [ -f "${PROJECT_ROOT}/fast-export2.pro" ]; then
		if (cd "${PROJECT_ROOT}" && qmake && make -j"$(nproc)") >> "${LOG_FILE}" 2>&1 \
			&& [ -x "${PROJECT_ROOT}/svn-all-fast-export" ]; then
			SVN2GIT_BINARY="${PROJECT_ROOT}/svn-all-fast-export"
			log "✓ Successfully built svn-all-fast-export"
		else
			error "qmake build failed — see ${LOG_FILE}"
		fi
	else
		error "svn-all-fast-export not found and qmake unavailable. Install qtbase5-dev qt5-qmake libsvn-dev libapr1-dev, then run 'qmake && make' at the repo root."
	fi
fi
log "Using converter: ${SVN2GIT_BINARY}"

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
	error "Authors file has only ${AUTHOR_COUNT} entries, expected at least 40"
fi
log "✓ Authors file validated (${AUTHOR_COUNT} entries)"

log "Validating .rules file..."
if ! grep -q "create repository" "${SCRIPT_DIR}/${RULES_FILE}"; then
	error "Rules file is missing 'create repository' statement"
fi
if ! grep -q "match /trunk" "${SCRIPT_DIR}/${RULES_FILE}"; then
	error "Rules file is missing trunk match rule"
fi
log "✓ Rules file validated"

# Run svn2git
log "Running svn2git migration..."
MIGRATION_START=$(date +%s)

# Change to output directory for Git repository creation
mkdir -p "${GIT_OUTPUT_DIR}"
cd "${GIT_OUTPUT_DIR}"

# The converter opens the repository directly via libsvn — it takes a
# local filesystem path, NOT a file:// URL.
log "Command: ${SVN2GIT_BINARY} --identity-map ${SCRIPT_DIR}/${AUTHORS_FILE} --rules ${SCRIPT_DIR}/${RULES_FILE} ${SVN_REPO_PATH}"

if "${SVN2GIT_BINARY}" \
	--identity-map "${SCRIPT_DIR}/${AUTHORS_FILE}" \
	--rules "${SCRIPT_DIR}/${RULES_FILE}" \
	"${SVN_REPO_PATH}" \
	>> "${LOG_FILE}" 2>&1; then
	MIGRATION_END=$(date +%s)
	MIGRATION_DURATION=$((MIGRATION_END - MIGRATION_START))
	log "✓ Migration completed successfully (${MIGRATION_DURATION}s)"
else
	error "Migration failed!"
fi

# Verify Git repository was created (svn2git creates it in current directory based on .rules)
ACTUAL_GIT_REPO_PATH="${GIT_OUTPUT_DIR}/platform-git"
if [ ! -d "${ACTUAL_GIT_REPO_PATH}" ]; then
	error "Git repository not created at ${ACTUAL_GIT_REPO_PATH}"
fi
log "✓ Git repository created at ${ACTUAL_GIT_REPO_PATH}"

# Generate traceability artifacts (mapping JSON + SQLite) with the
# svn2git-validate CLI when available; it writes to its current directory.
VALIDATE_BIN="${PROJECT_ROOT}/build/svn2git-validate"
if [ -x "${VALIDATE_BIN}" ]; then
	log "Generating traceability map with svn2git-validate..."
	if (cd "${ACTUAL_GIT_REPO_PATH}" && "${VALIDATE_BIN}" --generate-traceability-map \
		--git-repo "${ACTUAL_GIT_REPO_PATH}" --operator testengr) >> "${LOG_FILE}" 2>&1; then
		log "✓ Traceability artifacts generated (svn_to_git_mapping.json, traceability.db)"
	else
		log "⚠ Traceability generation failed (non-fatal)"
	fi
else
	log "⚠ svn2git-validate not built — skipping traceability artifacts"
fi

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
