# SVN2Git Comprehensive Test Framework

A complete, engineer-friendly shell script framework for validating `svn2git` tool functionality end-to-end. Creates a realistic 10,000+ revision SVN repository with 200 branches, support branches with tags, and 50 unique authors—then migrates it to Git and validates the output.

## Quick Start

```bash
# Make scripts executable
chmod +x *.sh

# Run complete test (default: cleanup afterward)
./test_svn2git.sh

# Iterative testing (keep artifacts)
./test_svn2git.sh --keep-svn --keep-git

# Debug specific stage
./test_svn2git.sh --skip-svn --verbose
```

## Overview

This test framework validates that `svn2git` correctly:
- Creates multiple Git repositories from a single SVN source
- Routes branches to correct repositories
- Creates tags only from support branches
- Preserves all commits with correct authors
- Maintains file content and structure
- Generates traceability mappings
- Produces audit trails

## Files in This Directory

| File | Purpose |
|------|---------|
| `test_svn2git.sh` | Master orchestrator - runs complete test pipeline |
| `setup_svn_repo.sh` | Creates realistic SVN repository (10,000+ revisions) |
| `run_migration.sh` | Executes SVN2Git migration with validation |
| `validate_migration.sh` | Post-migration verification and reporting |
| `generate_test_data.sh` | Generates C/H/CMake test files |
| `config.env` | Configuration parameters (paths, counts, timeouts) |
| `authors.txt` | 50 SVN-to-Git author mappings |
| `.rules` | SVN2Git rules file (path → repository/branch routing) |
| `README.md` | This file |

## Configuration

Edit `config.env` to adjust test parameters:

```bash
# Repository paths
SVN_ROOT_DIR="/mnt/svn/CV"
SVN_REPO_NAME="platform"

GIT_OUTPUT_DIR="/mnt/git"
GIT_REPO_NAME="platform-git"

# SVN configuration
TOTAL_REVISIONS=10247
NUM_REGULAR_BRANCHES=51
NUM_SUPPORT_BRANCHES=10
NUM_AUTHORS=50

# Commit distribution (realistic)
TRUNK_COMMITS=4000           # 40% on trunk
FEATURE_BRANCHES_COMMITS=5000 # 50% on feature branches
SUPPORT_BRANCHES_COMMITS=1247 # 10% on support branches

# Disk space requirement
REQUIRED_DISK_SPACE=5120     # 5GB in MB

# Timeouts (in seconds)
SVN_SETUP_TIMEOUT=600        # 10 minutes
MIGRATION_TIMEOUT=900        # 15 minutes
VALIDATION_TIMEOUT=300       # 5 minutes
```

## Repository Structure

The test creates a realistic SVN repository:

```
/mnt/svn/CV/platform/
├── trunk/              # Main development (4,000 commits)
│   ├── src/
│   │   ├── module_1.c
│   │   ├── module_2.c
│   │   └── ... (10 .c files)
│   ├── include/
│   │   ├── module_1.h
│   │   ├── module_2.h
│   │   └── ... (10 .h files)
│   ├── CMakeLists.txt
│   ├── build/
│   └── Makefile
│
├── branches/           # Feature & support branches
│   ├── platform-1000   # Feature branches (platform-1000 to platform-1050)
│   ├── platform-1001
│   ├── ...
│   ├── platform-bl102  # Support/maintenance branches (platform-bl*)
│   ├── platform-bl103
│   └── ...
│
└── tags/               # Tags (ONLY from support branches)
    ├── PLATFORM-BL102-v1
    ├── PLATFORM-BL102-v2
    ├── PLATFORM-BL102-v3
    ├── PLATFORM-BL103-v1
    └── ...
```

Authors: All 50 authors from `authors.txt` appear in commit history.

## Usage Examples

### 1. Complete Test (Default)
```bash
./test_svn2git.sh
```
- Creates SVN repository
- Runs migration
- Validates results
- Cleans up (removes test artifacts)
- Generates report

### 2. Iterative Testing
```bash
./test_svn2git.sh --keep-svn --keep-git
```
Keep test artifacts for inspection/debugging. Allows re-running migration without recreating SVN.

### 3. Skip SVN Setup (Reuse Existing)
```bash
./test_svn2git.sh --skip-svn --verbose
```
- Uses previously created SVN repo
- Useful when testing migration changes
- Saves time during iterative development

### 4. Run Validation Only
```bash
./test_svn2git.sh --skip-svn --skip-migrate
```
- Tests validation logic independently
- Doesn't create SVN or run migration

### 5. Clean Mode
```bash
./test_svn2git.sh --clean
```
- Automatically removes previous test runs
- Useful for CI/automated testing (non-interactive)

### 6. Verbose Logging
```bash
./test_svn2git.sh --verbose
```
- Shows detailed progress for each stage
- Useful for debugging

## Test Stages

### Stage 1: Environment Setup
- Checks prerequisites (svn, git, svnadmin)
- Verifies disk space
- Prompts for cleanup if repos exist
- Creates logging directory

### Stage 2: SVN Repository Creation (`setup_svn_repo.sh`)
**Duration**: ~3-5 minutes

Creates:
- Standard SVN layout (trunk/branches/tags)
- 10,247 revisions distributed as:
  - Trunk: 4,000 commits (40%)
  - Feature branches: 5,000 commits (50%)
  - Support branches: 1,247 commits (10%)
- 51 feature branches (platform-1000 to platform-1050)
- 10 support branches (platform-bl102 to platform-bl111)
- ~30 tags (created only on support branches)
- All 50 authors participating in commits
- C source files, header files, CMakeLists.txt

**Output**:
- SVN repository at `/mnt/svn/CV/platform/`
- Pre-migration report: `svn_pre_migration_report.txt`
- SVN info: `svn_repo_info.txt`

### Stage 3: SVN2Git Migration (`run_migration.sh`)
**Duration**: ~8-10 minutes

Performs:
- Validates `authors.txt` (50 authors)
- Validates `.rules` file (syntax/semantics)
- Executes svn2git with:
  - Traceability mapping enabled
  - JSON export of SVN→Git mapping
  - SQLite database for queries
  - Audit trail with operator ID
- Captures migration logs

**Output**:
- Git repository at `/mnt/git/platform-git/`
- `svn_to_git_mapping.json` (revision mapping)
- `traceability.db` (SQLite queryable mapping)
- `migration_info.txt` (execution report)
- `migration.log` (detailed output)

### Stage 4: Validation (`validate_migration.sh`)
**Duration**: ~1-2 minutes

Checks:
- Git repository integrity (`git fsck`)
- Commit count (should match SVN)
- Branch naming conventions
- Tag naming (PLATFORM-BLXXX format, support-branches only)
- Master branch existence
- Content files present (C/H/CMake)
- Traceability files exist
- Comparison report (SVN vs Git metrics)

**Output**:
- `validation_report.txt` (detailed results)
- `validation.log` (execution log)

### Stage 5: Cleanup
- Removes SVN repo (unless `--keep-svn`)
- Removes Git repo (unless `--keep-git`)
- Archives logs

### Stage 6: Report Generation
Produces `FINAL_REPORT.txt` with:
- Test duration breakdown
- Repository paths
- Test results (PASSED/SKIPPED)
- Data preservation status
- Log locations

## Output & Logs

All test outputs go to: `/tmp/svn2git_test_logs/test_YYYYMMDD_HHMMSS/`

```
test_20260703_142345/
├── FINAL_REPORT.txt          # Overall summary
├── master.log                 # Master orchestrator log
├── svn_setup.log              # SVN creation log
├── svn_repo_info.txt          # SVN statistics
├── svn_pre_migration_report.txt # Pre-migration baseline
├── migration.log              # Migration execution log
├── migration_info.txt         # Migration statistics
├── validation.log             # Validation log
├── validation_report.txt      # Validation results
└── timings.txt                # Performance metrics
```

## Performance Metrics

Typical execution on standard hardware:

| Stage | Duration | Notes |
|-------|----------|-------|
| SVN Setup | 3-5 min | Generates 10,000+ commits |
| Migration | 8-10 min | Depends on file I/O and svn2git performance |
| Validation | 1-2 min | Integrity checks and reporting |
| **Total** | **12-17 min** | Excluding cleanup |

**Repository Sizes**:
- SVN: ~500MB
- Git: ~300MB (60% compression vs SVN)

## Validation Checklist

The test verifies:

- ✓ SVN repo structure (trunk/branches/tags)
- ✓ Exactly 10,000+ revisions generated
- ✓ 51 feature branches with correct naming
- ✓ 10 support branches
- ✓ ~30 tags created only from support branches
- ✓ 50 unique authors in history
- ✓ C source files (.c) present
- ✓ Header files (.h) present
- ✓ CMakeLists.txt present
- ✓ Migration completes without errors
- ✓ Git branch count matches SVN
- ✓ Git commit count matches SVN
- ✓ Git tag count matches SVN
- ✓ `git fsck` reports no corruption
- ✓ Traceability mapping is complete (1:1)
- ✓ All test artifacts cleanable

## Prerequisites

### Required Tools
- `svnadmin` (Apache Subversion admin)
- `svn` (Subversion client)
- `git` (Git version control)
- `bash` (shell interpreter)

### System Requirements
- **Disk Space**: 5GB minimum
- **Memory**: 2GB minimum recommended
- **OS**: Linux or macOS (Unix-like environment)
- **Architecture**: x86_64

### Installation (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install subversion git build-essential cmake
```

### Installation (macOS)
```bash
brew install subversion git cmake
```

## Advanced Usage

### Run Specific Stage
```bash
# Skip everything, just validate
./test_svn2git.sh --skip-svn --skip-migrate

# Skip to migration (reuse SVN from previous run)
./test_svn2git.sh --skip-svn --keep-git
```

### CI/Automated Testing
```bash
./test_svn2git.sh --clean --keep-svn --verbose 2>&1 | tee test_output.log
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ]; then
  echo "✓ SVN2Git validation PASSED"
else
  echo "✗ SVN2Git validation FAILED"
fi
exit $EXIT_CODE
```

### Debugging
1. Enable verbose mode: `./test_svn2git.sh --verbose`
2. Keep artifacts: `./test_svn2git.sh --keep-svn --keep-git`
3. Check logs in test session directory
4. Manually inspect SVN/Git repos for debugging

## Customization

### Adjust Repository Size
Edit `config.env`:
```bash
TOTAL_REVISIONS=20000         # Larger test
NUM_REGULAR_BRANCHES=100      # More branches
NUM_AUTHORS=100               # More authors
```

### Change Directory Paths
Edit `config.env`:
```bash
SVN_ROOT_DIR="/custom/svn/path"
GIT_OUTPUT_DIR="/custom/git/path"
LOG_DIR="/custom/logs/path"
```

### Modify Rules File
Edit `.rules` to change routing logic:
```
# Example: route different branches to different repos
create repository platform-core
create repository platform-experimental

match /trunk
  repository platform-core
  branch master
  end

match /branches/platform-exp-.*
  repository platform-experimental
  branch refs/heads/\1
  end
```

## Troubleshooting

### "svnadmin not found"
- Install Subversion: `sudo apt-get install subversion` (Ubuntu/Debian)
- Or: `brew install subversion` (macOS)

### "Permission denied" on scripts
- Make executable: `chmod +x *.sh`

### "Insufficient disk space"
- Free up 5GB or edit `REQUIRED_DISK_SPACE` in `config.env`
- Check disk: `df -h /mnt/svn /mnt/git`

### "Directory already exists" (SVN/Git)
- Use `--clean` flag to auto-remove
- Or manually remove: `rm -rf /mnt/svn/CV /mnt/git/`
- Or use `--skip-svn` to reuse existing repos

### Migration fails
1. Check logs: `tail -f /tmp/svn2git_test_logs/test_*/migration.log`
2. Run with verbose: `./test_svn2git.sh --verbose`
3. Validate svn2git binary: `which svn2git` or build from source
4. Check .rules file syntax: Look for errors in `migration.log`

### Tests take too long
- Normal: 12-17 minutes on standard hardware
- If >20 minutes: Check disk I/O with `iostat`
- Reduce TOTAL_REVISIONS if testing quickly (testing, not benchmarking)

## Contributing

To improve the test framework:
1. Test changes locally: `./test_svn2git.sh --keep-svn --keep-git`
2. Run multiple times to verify consistency
3. Document any new configuration options
4. Test edge cases (empty branches, merged branches, etc.)

## License

Same as svn2git project

## Questions?

- Check logs in test session directory
- Run with `--verbose` for detailed output
- Review `FINAL_REPORT.txt` for summary
