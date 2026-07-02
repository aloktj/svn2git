/*
 *  svn2git enhanced — RevisionMapper unit tests (Catch2)
 */

#include "svn2git/revision_mapper.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using svn2git::RevisionMapper;

namespace {

/// 40-hex-digit SHA derived from a small integer, for readable tests.
std::string fakeSha(int seed)
{
    std::ostringstream out;
    out << std::hex << seed;
    std::string sha = out.str();
    while (sha.size() < 40)
        sha = "0" + sha;
    return sha;
}

struct FileGuard {
    std::string path;
    ~FileGuard() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("recording a single mapping enables bidirectional lookup", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(0xabc), "John Smith", "2024-01-15T10:30:00Z",
                         "initial import");

    REQUIRE(mapper.size() == 1);
    CHECK(mapper.findGitCommitBySvnRevision(1) == fakeSha(0xabc));
    CHECK(mapper.findSvnRevisionByGitCommit(fakeSha(0xabc)) == 1);
}

TEST_CASE("multiple mappings are all retrievable", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    for (int revision = 1; revision <= 25; ++revision)
        mapper.recordMapping(revision, fakeSha(revision), "Author",
                             "2024-01-15T10:30:00Z", "commit");

    REQUIRE(mapper.size() == 25);
    for (int revision = 1; revision <= 25; ++revision)
        CHECK(mapper.findGitCommitBySvnRevision(revision) == fakeSha(revision));
}

TEST_CASE("unknown lookups return sentinel values", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");

    CHECK(mapper.findGitCommitBySvnRevision(999).empty());
    CHECK(mapper.findSvnRevisionByGitCommit(fakeSha(0xdead)) == -1);
    CHECK(mapper.findSvnRevisionByGitCommit("short") == -1); // <7 chars
    CHECK(mapper.findSvnRevisionByGitCommit("nothex!") == -1);
}

TEST_CASE("unambiguous SHA prefixes resolve, ambiguous ones do not", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    // Two SHAs sharing a long common prefix.
    const std::string shaA = std::string(39, 'a') + "1";
    const std::string shaB = std::string(39, 'a') + "2";
    mapper.recordMapping(1, shaA, "A", "2024-01-01T00:00:00Z", "m1");
    mapper.recordMapping(2, shaB, "A", "2024-01-01T00:00:00Z", "m2");

    CHECK(mapper.findSvnRevisionByGitCommit(shaA.substr(0, 40)) == 1);
    CHECK(mapper.findSvnRevisionByGitCommit(std::string(12, 'a')) == -1); // ambiguous
}

TEST_CASE("duplicate SVN revision fails 1:1 validation", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");
    mapper.recordMapping(1, fakeSha(2), "A", "2024-01-01T00:00:00Z", "m");

    CHECK(mapper.size() == 1); // first recording wins
    CHECK_FALSE(mapper.validateOneToOneMapping());
}

TEST_CASE("duplicate Git SHA fails 1:1 validation", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(7), "A", "2024-01-01T00:00:00Z", "m");
    mapper.recordMapping(2, fakeSha(7), "A", "2024-01-01T00:00:00Z", "m");

    CHECK_FALSE(mapper.validateOneToOneMapping());
}

TEST_CASE("malformed mappings are rejected and fail validation", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(-5, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");
    mapper.recordMapping(1, "not-a-sha", "A", "2024-01-01T00:00:00Z", "m");

    CHECK(mapper.size() == 0);
    CHECK_FALSE(mapper.validateOneToOneMapping());
}

TEST_CASE("gap detection distinguishes skipped from missing revisions",
          "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");
    mapper.recordMapping(4, fakeSha(4), "A", "2024-01-01T00:00:00Z", "m");
    mapper.recordSkippedRevision(2, "touches only ignored paths");

    const std::vector<long> missing = mapper.missingRevisions();
    REQUIRE(missing.size() == 1);
    CHECK(missing.front() == 3);
    CHECK_FALSE(mapper.validateOneToOneMapping());

    mapper.recordSkippedRevision(3, "empty revision");
    CHECK(mapper.missingRevisions().empty());
    CHECK(mapper.validateOneToOneMapping());
}

TEST_CASE("contiguous complete mapping passes 1:1 validation", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    for (int revision = 1; revision <= 10; ++revision)
        mapper.recordMapping(revision, fakeSha(revision), "A", "2024-01-01T00:00:00Z",
                             "m");
    CHECK(mapper.validateOneToOneMapping());
}

TEST_CASE("empty mapper fails 1:1 validation", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    CHECK_FALSE(mapper.validateOneToOneMapping());
}

TEST_CASE("JSON export matches the documented schema", "[revision-mapper]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-mapping", ".json")};

    RevisionMapper mapper("freertos24");
    mapper.recordMapping(12345, fakeSha(0xf00d), "John Smith", "2024-01-15T10:30:00Z",
                         "Fix pantograph ECU firmware\n\nlong body text");
    REQUIRE(mapper.generateMappingFile(guard.path));

    std::ifstream file(guard.path);
    REQUIRE(file.is_open());
    const nlohmann::json root = nlohmann::json::parse(file);

    CHECK(root.at("repository") == "freertos24");
    CHECK(root.at("total_mappings") == 1);
    REQUIRE(root.at("mappings").size() == 1);

    const nlohmann::json& entry = root.at("mappings").front();
    CHECK(entry.at("svn_revision") == 12345);
    CHECK(entry.at("git_commit_sha") == fakeSha(0xf00d));
    CHECK(entry.at("git_short_sha") == fakeSha(0xf00d).substr(0, 7));
    CHECK(entry.at("author") == "John Smith");
    CHECK(entry.at("timestamp") == "2024-01-15T10:30:00Z");
    // Snippet is single-line.
    const std::string snippet = entry.at("message_snippet");
    CHECK(snippet == "Fix pantograph ECU firmware");
}

TEST_CASE("JSON export fails cleanly on unwritable path", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");
    CHECK_FALSE(mapper.generateMappingFile("/nonexistent-dir/mapping.json"));
}

TEST_CASE("SQLite export creates a queryable traceability database", "[revision-mapper]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-trace", ".db")};

    RevisionMapper mapper("testrepo");
    for (int revision = 1; revision <= 5; ++revision)
        mapper.recordMapping(revision, fakeSha(revision),
                             "Author " + std::to_string(revision), "2024-01-01T00:00:00Z",
                             "commit " + std::to_string(revision));
    REQUIRE(mapper.generateMappingDatabase(guard.path));

    // Verify with a direct SQLite query.
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(guard.path.c_str(), &raw) == SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);

    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(
                db.get(), "SELECT git_commit_sha FROM mappings WHERE svn_revision = 3",
                -1, &stmt, nullptr)
            == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const unsigned char* sha = sqlite3_column_text(stmt, 0);
    REQUIRE(sha != nullptr);
    CHECK(std::string(reinterpret_cast<const char*>(sha)) == fakeSha(3));
    sqlite3_finalize(stmt);

    // Row count must equal the number of mappings.
    REQUIRE(
        sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM mappings", -1, &stmt, nullptr)
        == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 5);
    sqlite3_finalize(stmt);
}

TEST_CASE("SQLite export fails cleanly on unwritable path", "[revision-mapper]")
{
    RevisionMapper mapper("testrepo");
    mapper.recordMapping(1, fakeSha(1), "A", "2024-01-01T00:00:00Z", "m");
    CHECK_FALSE(mapper.generateMappingDatabase("/nonexistent-dir/trace.db"));
}
