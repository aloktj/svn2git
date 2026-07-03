#!/bin/bash
# SVN2Git Comprehensive Test Suite - Master Orchestrator
# Single-script execution for complete end-to-end testing

set -e

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory and configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.env"

# Global variables
SKIP_SVN_SETUP=false
SKIP_MIGRATION=false
SKIP_VALIDATION=false
VERBOSE=false
KEEP_SVN=false
KEEP_GIT=false
CLEAN_MODE=false

# Initialize test session
TEST_SESSION_DIR="${LOG_DIR}/test_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${TEST_SESSION_DIR}"
MASTER_LOG="${TEST_SESSION_DIR}/master.log"

# Functions
print_banner() {
	cat << 'EOF'
╔══════════════════════════════════════════════════════════════╗
║         SVN2Git Comprehensive Test Framework                ║
║       End-to-End Migration Validation & Testing              ║
╚══════════════════════════════════════════════════════════════╝
EOF
}

print_header() {
	echo -e "${BLUE}==> $1${NC}"
}

print_success() {
	echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
	echo -e "${RED}✗ $1${NC}"
}

print_warning() {
	echo -e "${YELLOW}⚠ $1${NC}"
}

print_progress() {
	echo -e "${BLUE}[*] $1${NC}"
}

log() {
	echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" >> "${MASTER_LOG}"
	if [ "${VERBOSE}" = "true" ]; then
		echo "[LOG] $*"
	fi
}

show_usage() {
	cat << EOF
Usage: $0 [OPTIONS]

Options:
  --skip-svn          Skip SVN repository creation (use existing)
  --skip-migrate      Skip migration (use existing Git output)
  --skip-validation   Skip validation checks
  --clean             Clean previous test runs before starting
  --keep-svn          Preserve SVN repository after test
  --keep-git          Preserve Git output after test
  --verbose, -v       Show detailed progress logs
  --help, -h          Show this help message

Examples:
  # Complete test with cleanup
  $0

  # Iterative testing (keep artifacts)
  $0 --keep-svn --keep-git

  # Debug specific stage
  $0 --skip-svn --verbose

  # Run validation only
  $0 --skip-svn --skip-migrate

EOF
}

parse_arguments() {
	while [[ $# -gt 0 ]]; do
		case $1 in
		--skip-svn)
			SKIP_SVN_SETUP=true
			shift
			;;
		--skip-migrate)
			SKIP_MIGRATION=true
			shift
			;;
		--skip-validation)
			SKIP_VALIDATION=true
			shift
			;;
		--keep-svn)
			KEEP_SVN=true
			shift
			;;
		--keep-git)
			KEEP_GIT=true
			shift
			;;
		--clean)
			CLEAN_MODE=true
			shift
			;;
		--verbose | -v)
			VERBOSE=true
			shift
			;;
		--help | -h)
			show_usage
			exit 0
			;;
		*)
			print_error "Unknown option: $1"
			show_usage
			exit 1
			;;
		esac
	done
}

check_prerequisites() {
	print_header "Checking Prerequisites"

	# Check required tools
	local missing_tools=()

	if ! command -v svnadmin &>/dev/null; then
		missing_tools+=("svnadmin")
	else
		print_success "svnadmin found"
	fi

	if ! command -v svn &>/dev/null; then
		missing_tools+=("svn")
	else
		print_success "svn client found"
	fi

	if ! command -v git &>/dev/null; then
		missing_tools+=("git")
	else
		print_success "git found"
	fi

	if [ ${#missing_tools[@]} -gt 0 ]; then
		print_error "Missing required tools: ${missing_tools[*]}"
		echo "  Please install: ${missing_tools[*]}"
		exit 1
	fi

	# Check disk space (df returns 1K blocks by default, convert to MB)
	print_progress "Checking disk space..."
	AVAILABLE_SPACE_KB=$(df "${SVN_ROOT_DIR%/*}" 2>/dev/null | awk 'NR==2 {print $4}')
	AVAILABLE_SPACE=$((AVAILABLE_SPACE_KB / 1024))
	if [ -z "${AVAILABLE_SPACE_KB}" ]; then
		print_warning "Could not determine available disk space"
	elif [ "${AVAILABLE_SPACE}" -lt "${REQUIRED_DISK_SPACE}" ]; then
		print_error "Insufficient disk space. Required: ${REQUIRED_DISK_SPACE}MB, Available: ${AVAILABLE_SPACE}MB"
		exit 1
	else
		print_success "Sufficient disk space (${AVAILABLE_SPACE}MB available)"
	fi

	log "Prerequisites check passed"
}

setup_environment() {
	print_header "Setting Up Test Environment"

	# Create directories
	mkdir -p "${SVN_ROOT_DIR}"
	mkdir -p "${GIT_OUTPUT_DIR}"
	mkdir -p "${LOG_DIR}"

	print_progress "Test session: ${TEST_SESSION_DIR}"
	log "Test session directory created"

	# Handle cleanup if needed
	if [ "${CLEAN_MODE}" = "true" ]; then
		print_warning "Clean mode enabled - removing previous test runs"
		if [ -d "${SVN_REPO_PATH}" ]; then
			print_progress "Removing SVN repository..."
			rm -rf "${SVN_REPO_PATH}"
			log "SVN repository removed"
		fi
		if [ -d "${GIT_REPO_PATH}" ]; then
			print_progress "Removing Git repository..."
			rm -rf "${GIT_REPO_PATH}"
			log "Git repository removed"
		fi
	else
		# Interactive cleanup
		if [ -d "${SVN_REPO_PATH}" ] && [ "${SKIP_SVN_SETUP}" = "false" ]; then
			echo -n "SVN repository already exists. Remove it? [y/N] "
			read -r response
			if [[ "${response}" =~ ^[Yy]$ ]]; then
				rm -rf "${SVN_REPO_PATH}"
				log "SVN repository removed (user confirmed)"
			else
				print_warning "Using existing SVN repository"
				SKIP_SVN_SETUP=true
			fi
		fi

		if [ -d "${GIT_REPO_PATH}" ] && [ "${SKIP_MIGRATION}" = "false" ]; then
			echo -n "Git repository already exists. Remove it? [y/N] "
			read -r response
			if [[ "${response}" =~ ^[Yy]$ ]]; then
				rm -rf "${GIT_REPO_PATH}"
				log "Git repository removed (user confirmed)"
			else
				print_warning "Using existing Git repository"
				SKIP_MIGRATION=true
			fi
		fi
	fi

	print_success "Environment setup complete"
	log "Environment setup completed"
}

run_svn_setup() {
	if [ "${SKIP_SVN_SETUP}" = "true" ]; then
		print_warning "Skipping SVN repository creation (--skip-svn)"
		return 0
	fi

	print_header "Creating SVN Repository"

	if [ ! -x "${SCRIPT_DIR}/setup_svn_repo.sh" ]; then
		print_error "setup_svn_repo.sh not found or not executable"
		exit 1
	fi

	# Export test session directory for child scripts
	export TEST_SESSION_DIR

	SVN_START=$(date +%s)
	if bash "${SCRIPT_DIR}/setup_svn_repo.sh"; then
		SVN_END=$(date +%s)
		SVN_DURATION=$((SVN_END - SVN_START))
		print_success "SVN repository created (${SVN_DURATION}s)"
		log "SVN repository creation successful (${SVN_DURATION}s)"

		# Save timing
		echo "SVN_SETUP_DURATION=${SVN_DURATION}" >> "${TEST_SESSION_DIR}/timings.txt"
	else
		print_error "SVN repository creation failed"
		log "SVN repository creation FAILED"
		exit 1
	fi
}

run_migration() {
	if [ "${SKIP_MIGRATION}" = "true" ]; then
		print_warning "Skipping migration (--skip-migrate)"
		return 0
	fi

	print_header "Running SVN2Git Migration"

	if [ ! -x "${SCRIPT_DIR}/run_migration.sh" ]; then
		print_error "run_migration.sh not found or not executable"
		exit 1
	fi

	# Export test session directory for child scripts
	export TEST_SESSION_DIR

	MIGRATION_START=$(date +%s)
	if bash "${SCRIPT_DIR}/run_migration.sh"; then
		MIGRATION_END=$(date +%s)
		MIGRATION_DURATION=$((MIGRATION_END - MIGRATION_START))
		print_success "Migration completed (${MIGRATION_DURATION}s)"
		log "Migration successful (${MIGRATION_DURATION}s)"

		# Save timing
		echo "MIGRATION_DURATION=${MIGRATION_DURATION}" >> "${TEST_SESSION_DIR}/timings.txt"
	else
		print_error "Migration failed"
		log "Migration FAILED"
		exit 1
	fi
}

run_validation() {
	if [ "${SKIP_VALIDATION}" = "true" ]; then
		print_warning "Skipping validation (--skip-validation)"
		return 0
	fi

	print_header "Validating Migration Results"

	if [ ! -x "${SCRIPT_DIR}/validate_migration.sh" ]; then
		print_error "validate_migration.sh not found or not executable"
		exit 1
	fi

	# Export test session directory for child scripts
	export TEST_SESSION_DIR

	VALIDATION_START=$(date +%s)
	if bash "${SCRIPT_DIR}/validate_migration.sh"; then
		VALIDATION_END=$(date +%s)
		VALIDATION_DURATION=$((VALIDATION_END - VALIDATION_START))
		print_success "Validation passed (${VALIDATION_DURATION}s)"
		log "Validation successful (${VALIDATION_DURATION}s)"

		# Save timing
		echo "VALIDATION_DURATION=${VALIDATION_DURATION}" >> "${TEST_SESSION_DIR}/timings.txt"
	else
		print_error "Validation failed"
		log "Validation FAILED"
		exit 1
	fi
}

cleanup() {
	print_header "Cleanup"

	if [ "${KEEP_SVN}" = "false" ] && [ -d "${SVN_REPO_PATH}" ]; then
		print_progress "Removing SVN repository..."
		rm -rf "${SVN_REPO_PATH}"
		log "SVN repository removed"
	else
		print_warning "Preserving SVN repository (--keep-svn)"
	fi

	if [ "${KEEP_GIT}" = "false" ] && [ -d "${GIT_REPO_PATH}" ]; then
		print_progress "Removing Git repository..."
		rm -rf "${GIT_REPO_PATH}"
		log "Git repository removed"
	else
		print_warning "Preserving Git repository (--keep-git)"
	fi

	print_success "Cleanup complete"
	log "Cleanup completed"
}

generate_final_report() {
	print_header "Generating Final Report"

	REPORT_FILE="${TEST_SESSION_DIR}/FINAL_REPORT.txt"

	# Read timings if available
	TOTAL_START=$(date -d "${TEST_SESSION_DIR: -15}" +'%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "$(date)")
	TOTAL_END=$(date +'%Y-%m-%d %H:%M:%S')

	# Calculate total duration
	TOTAL_START_EPOCH=$(date -d "${TOTAL_START}" +%s 2>/dev/null || date +%s)
	TOTAL_END_EPOCH=$(date +%s)
	TOTAL_DURATION=$((TOTAL_END_EPOCH - TOTAL_START_EPOCH))

	cat > "${REPORT_FILE}" << EOF
========================================
SVN2Git Test Suite - FINAL REPORT
========================================
Date:               ${TOTAL_END}
Test Duration:      ${TOTAL_DURATION}s

Repository Paths:
  SVN Repository:   ${SVN_REPO_PATH}
  Git Repository:   ${GIT_REPO_PATH}
  Test Session:     ${TEST_SESSION_DIR}

Test Results:
  SVN Setup:        $([ "${SKIP_SVN_SETUP}" = "false" ] && echo "✓ PASSED" || echo "⊘ SKIPPED")
  Migration:        $([ "${SKIP_MIGRATION}" = "false" ] && echo "✓ PASSED" || echo "⊘ SKIPPED")
  Validation:       $([ "${SKIP_VALIDATION}" = "false" ] && echo "✓ PASSED" || echo "⊘ SKIPPED")

Repository Status:
  SVN Repo Exists:  $([ -d "${SVN_REPO_PATH}" ] && echo "✓ YES" || echo "✗ NO")
  Git Repo Exists:  $([ -d "${GIT_REPO_PATH}" ] && echo "✓ YES" || echo "✗ NO")

Data Preservation:
  SVN Preserved:    $([ "${KEEP_SVN}" = "true" ] && echo "✓ YES" || echo "✗ REMOVED")
  Git Preserved:    $([ "${KEEP_GIT}" = "true" ] && echo "✓ YES" || echo "✗ REMOVED")

Command Flags Used:
  --skip-svn:       ${SKIP_SVN_SETUP}
  --skip-migrate:   ${SKIP_MIGRATION}
  --skip-validation: ${SKIP_VALIDATION}
  --keep-svn:       ${KEEP_SVN}
  --keep-git:       ${KEEP_GIT}
  --clean:          ${CLEAN_MODE}
  --verbose:        ${VERBOSE}

Logs Location:
  Master Log:       ${MASTER_LOG}
  Session Dir:      ${TEST_SESSION_DIR}

========================================
EOF

	cat "${REPORT_FILE}"
	log "Final report generated"

	# Display summary
	echo ""
	echo -e "${GREEN}========================================${NC}"
	echo -e "${GREEN}Test Suite Execution Summary${NC}"
	echo -e "${GREEN}========================================${NC}"
	echo -e "Result: ${GREEN}✓ ALL TESTS PASSED${NC}"
	echo "Logs: ${TEST_SESSION_DIR}"
	echo -e "${GREEN}========================================${NC}"
}

main() {
	print_banner
	echo ""

	# Parse command-line arguments
	parse_arguments "$@"

	# Track overall timing
	OVERALL_START=$(date +%s)

	# Run test stages
	check_prerequisites
	setup_environment
	run_svn_setup
	run_migration
	run_validation

	# Cleanup (optional)
	cleanup

	# Generate report
	OVERALL_END=$(date +%s)
	OVERALL_DURATION=$((OVERALL_END - OVERALL_START))
	log "Overall test duration: ${OVERALL_DURATION}s"

	generate_final_report

	echo ""
	print_success "SVN2Git test suite completed successfully!"
	exit 0
}

# Error handling
trap 'print_error "Test interrupted"; exit 1' INT TERM

# Run main
main "$@"
