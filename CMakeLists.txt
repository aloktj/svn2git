# svn2git enhanced — CMake build system
#
# Builds the Phase 1/2 validation & compliance tooling:
#   - svn2git-core        static library with all validators
#   - svn2git-validate    CLI front end (src/svn_to_git_app.cpp)
#   - unit + integration test suites (Catch2, enabled with BUILD_TESTING)
#
# The classic Qt-based converter (svn-all-fast-export) keeps its qmake
# build (src/src.pro); it is intentionally not part of this CMake build
# so the validation tooling stays free of a Qt dependency.

cmake_minimum_required(VERSION 3.16)

project(svn2git-enhanced
    VERSION 1.0.0
    DESCRIPTION "SVN to Git/GitLab migration validation and EN 50128 compliance tooling"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Strict warnings — treat them as errors so quality gates hold in CI.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# --- Dependencies -----------------------------------------------------------

find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# --- Core library -----------------------------------------------------------

add_library(svn2git-core STATIC
    src/logging.cpp
    src/command_runner.cpp
    src/error_reporter.cpp
    src/author_validator.cpp
    src/rules_validator.cpp
    src/config_validator.cpp
    src/revision_mapper.cpp
    src/audit_logger.cpp
    src/svn_validator.cpp
    src/git_validator.cpp
    src/content_validator.cpp)

target_include_directories(svn2git-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(svn2git-core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    SQLite::SQLite3
    Threads::Threads)

# --- CLI application --------------------------------------------------------

add_executable(svn2git-validate src/svn_to_git_app.cpp)
target_link_libraries(svn2git-validate PRIVATE svn2git-core)

install(TARGETS svn2git-validate RUNTIME DESTINATION bin)

# --- Tests ------------------------------------------------------------------

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
