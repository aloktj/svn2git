# svn2git enhanced — Catch2 test configuration
#
# Two suites:
#   svn2git-unit-tests         pure unit tests with fake command runners
#   svn2git-integration-tests  end-to-end tests that create real SVN/Git
#                              repositories (require svn, svnadmin, git
#                              on PATH; skipped gracefully when absent)

find_package(Catch2 3 REQUIRED)

# Fixtures directory is compiled in so tests find their data regardless
# of the working directory ctest chooses.
set(SVN2GIT_FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_executable(svn2git-unit-tests
    unit/test_author_validator.cpp
    unit/test_rules_validator.cpp
    unit/test_error_reporter.cpp
    unit/test_config_validator.cpp
    unit/test_revision_mapper.cpp
    unit/test_audit_logger.cpp)

target_link_libraries(svn2git-unit-tests PRIVATE
    svn2git-core
    Catch2::Catch2WithMain)
target_compile_definitions(svn2git-unit-tests PRIVATE
    SVN2GIT_FIXTURES_DIR="${SVN2GIT_FIXTURES_DIR}")

add_executable(svn2git-integration-tests
    integration/test_svn_validation.cpp
    integration/test_git_validation.cpp
    integration/test_full_migration.cpp)

target_link_libraries(svn2git-integration-tests PRIVATE
    svn2git-core
    Catch2::Catch2WithMain)
target_compile_definitions(svn2git-integration-tests PRIVATE
    SVN2GIT_FIXTURES_DIR="${SVN2GIT_FIXTURES_DIR}")

include(Catch)
catch_discover_tests(svn2git-unit-tests)
catch_discover_tests(svn2git-integration-tests)
