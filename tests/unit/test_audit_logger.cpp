/*
 *  svn2git enhanced — AuditLogger unit tests (Catch2)
 */

#include "svn2git/audit_logger.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using svn2git::AuditEvent;
using svn2git::AuditLogger;
using svn2git::CommandResult;

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

struct FileGuard {
    std::string path;
    ~FileGuard() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("session metadata is populated deterministically", "[audit-logger]")
{
    AuditLogger audit("freertos24", "john.smith@example.com", "prod-server-02");

    CHECK(audit.operatorId() == "john.smith@example.com");
    CHECK(audit.machine() == "prod-server-02");
    // mig-<14 digits>-<6 hex chars>
    const std::string& id = audit.migrationId();
    REQUIRE(id.rfind("mig-", 0) == 0);
    CHECK(id.size() == 4 + 14 + 1 + 6);
}

TEST_CASE("operator and machine fall back to environment detection", "[audit-logger]")
{
    AuditLogger audit("repo");
    CHECK_FALSE(audit.operatorId().empty());
    CHECK_FALSE(audit.machine().empty());
}

TEST_CASE("events are recorded in order with ISO timestamps", "[audit-logger]")
{
    AuditLogger audit("repo", "op", "host");
    audit.logEvent("Validation", "SVN repo integrity", "OK");
    audit.logEvent("Conversion", "starting fast-import", "");

    REQUIRE(audit.events().size() == 2);
    const AuditEvent& first = audit.events().front();
    CHECK(first.type == "Validation");
    CHECK(first.details == "SVN repo integrity");
    CHECK(first.status == "OK");
    REQUIRE(first.timestamp.size() == 20);
    CHECK(first.timestamp[10] == 'T');
    CHECK(first.timestamp.back() == 'Z');
}

TEST_CASE("milestones embed commit count and duration", "[audit-logger]")
{
    AuditLogger audit("repo", "op", "host");
    audit.logMilestone("trunk conversion", 892, 765000);

    REQUIRE(audit.events().size() == 1);
    const AuditEvent& event = audit.events().front();
    CHECK(event.type == "Milestone");
    CHECK(event.details.find("trunk conversion") != std::string::npos);
    CHECK(event.details.find("892 commit(s)") != std::string::npos);
    CHECK(event.details.find("765000 ms") != std::string::npos);
    CHECK(event.status == "OK");
}

TEST_CASE("audit trail renders header, events and outcome", "[audit-logger]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-audit", ".log")};

    AuditLogger audit("freertos24", "john.smith@example.com", "prod-02");
    audit.logEvent("Validation", "SVN repo integrity", "OK");
    audit.logMilestone("trunk", 892, 1000);
    audit.setOutcome(true, "1247 commits migrated");
    REQUIRE(audit.generateAuditTrail(guard.path));

    const std::string content = slurp(guard.path);
    CHECK(content.find("MIGRATION AUDIT TRAIL") != std::string::npos);
    CHECK(content.find("Repository: freertos24") != std::string::npos);
    CHECK(content.find("Migration ID: mig-") != std::string::npos);
    CHECK(content.find("Operator: john.smith@example.com") != std::string::npos);
    CHECK(content.find("Machine: prod-02") != std::string::npos);
    CHECK(content.find("Validation: SVN repo integrity... OK") != std::string::npos);
    CHECK(content.find("Completed: 1247 commits migrated... SUCCESS")
          != std::string::npos);
    // No missing-outcome warning when the outcome was set.
    CHECK(content.find("no final outcome") == std::string::npos);
}

TEST_CASE("missing outcome is flagged in the audit trail", "[audit-logger]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-audit-noend", ".log")};

    AuditLogger audit("repo", "op", "host");
    audit.logEvent("Validation", "step", "OK");
    REQUIRE(audit.generateAuditTrail(guard.path));

    CHECK(slurp(guard.path).find("no final outcome") != std::string::npos);
}

TEST_CASE("failure outcome is rendered as FAILURE", "[audit-logger]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-audit-fail", ".log")};

    AuditLogger audit("repo", "op", "host");
    audit.setOutcome(false, "3 errors");
    REQUIRE(audit.generateAuditTrail(guard.path));
    CHECK(slurp(guard.path).find("FAILURE") != std::string::npos);
}

TEST_CASE("audit trail write fails cleanly on unwritable path", "[audit-logger]")
{
    AuditLogger audit("repo", "op", "host");
    CHECK_FALSE(audit.generateAuditTrail("/nonexistent-dir/audit.log"));
}

TEST_CASE("signing requires an existing audit file", "[audit-logger]")
{
    AuditLogger audit("repo", "op", "host");
    CHECK_FALSE(audit.signAuditLog("key@example.com", "/nonexistent/audit.log"));
}

TEST_CASE("signing rejects an empty key id", "[audit-logger]")
{
    AuditLogger audit("repo", "op", "host");
    CHECK_FALSE(audit.signAuditLog("", "audit.log"));
}

TEST_CASE("signing invokes gpg through the runner", "[audit-logger]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-audit-sign", ".log")};

    std::string capturedCommand;
    auto fakeRunner = [&capturedCommand](const std::string& command) {
        capturedCommand = command;
        return CommandResult {0, ""};
    };

    AuditLogger audit("repo", "op", "host", fakeRunner);
    audit.setOutcome(true);
    REQUIRE(audit.generateAuditTrail(guard.path));
    REQUIRE(audit.signAuditLog("release@example.com", guard.path));

    CHECK(capturedCommand.find("gpg") != std::string::npos);
    CHECK(capturedCommand.find("--detach-sign") != std::string::npos);
    CHECK(capturedCommand.find("release@example.com") != std::string::npos);
    CHECK(capturedCommand.find(guard.path) != std::string::npos);
}

TEST_CASE("gpg failure propagates as false", "[audit-logger]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-audit-signf", ".log")};

    auto failingRunner = [](const std::string&) {
        return CommandResult {2, "gpg: signing failed: No secret key"};
    };

    AuditLogger audit("repo", "op", "host", failingRunner);
    audit.setOutcome(true);
    REQUIRE(audit.generateAuditTrail(guard.path));
    CHECK_FALSE(audit.signAuditLog("nokey@example.com", guard.path));
}
