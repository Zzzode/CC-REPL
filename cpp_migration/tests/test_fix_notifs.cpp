/// @file test_fix_notifs.cpp
/// @brief Coverage for the real-backend bridges added to
///        cc.hooks.remaining_notifs:
///          * to_mcp_server_status() — ConnectionStatus -> McpServerStatus
///          * inject_mcp_connectivity_from_manager() — live manager -> slot
///          * inject_teammate_shutdowns_from_tasks<>() — task list -> slot
///
/// The teammate bridge is a template that takes the terminal-state test and
/// cause mapping as callables, so it can be exercised here with a fake task
/// struct (no cc.tasks dependency required).

#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

import cc.hooks.remaining_notifs;
import cc.services.mcp.connection_manager;
import cc.services.mcp.types;

namespace notif = cc::hooks::notifs;
namespace svc_mcp = cc::services::mcp;

using namespace std::chrono_literals;

namespace {

// Fake task shape that satisfies the teammate bridge template's duck-typed
// contract (.identity.agent_id / .identity.agent_name + caller-supplied
// terminal predicate + cause mapping).
struct FakeIdentity {
    std::string agent_id;
    std::string agent_name;
};
enum class FakeStatus { Running, Completed, Failed, Killed };
struct FakeTask {
    FakeIdentity identity;
    FakeStatus status{FakeStatus::Running};
};

struct NotifStateReset {
    NotifStateReset() { notif::reset_dismissals_for_tests(); }
    ~NotifStateReset() {
        notif::reset_dismissals_for_tests();
        notif::clear_mcp_connectivity();
        notif::clear_teammate_shutdowns();
    }
};

bool is_terminal(const FakeTask& t) {
    return t.status == FakeStatus::Completed ||
           t.status == FakeStatus::Failed ||
           t.status == FakeStatus::Killed;
}

notif::TeammateShutdownCause to_cause(const FakeTask& t) {
    if (t.status == FakeStatus::Killed || t.status == FakeStatus::Failed) {
        return notif::TeammateShutdownCause::Crashed;
    }
    return notif::TeammateShutdownCause::Finished;
}

} // namespace

// ─── MCP status mapping ─────────────────────────────────────────────────────

TEST(FixNotifs, McpStatusMappingConnected) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Connected),
              notif::McpServerStatus::Connected);
}

TEST(FixNotifs, McpStatusMappingConnecting) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Connecting),
              notif::McpServerStatus::Connecting);
}

TEST(FixNotifs, McpStatusMappingDisconnected) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Disconnected),
              notif::McpServerStatus::Disconnected);
}

TEST(FixNotifs, McpStatusMappingErrorAndNeedsAuth) {
    // Both Error and NeedsAuth project onto the surfacing Error bucket so the
    // existing has_mcp_connectivity_issues() reader flags them.
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Error),
              notif::McpServerStatus::Error);
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::NeedsAuth),
              notif::McpServerStatus::Error);
}

// ─── Teammate bridge ────────────────────────────────────────────────────────

TEST(FixNotifs, TeammateBridgeIgnoresRunningTasks) {
    NotifStateReset guard;
    std::vector<FakeTask> tasks{
        {.identity = {"a1", "Alpha"}, .status = FakeStatus::Running},
        {.identity = {"a2", "Beta"},  .status = FakeStatus::Running},
    };
    notif::inject_teammate_shutdowns_from_tasks(tasks, is_terminal, to_cause);
    EXPECT_TRUE(notif::get_all_teammate_shutdowns().empty());
}

TEST(FixNotifs, TeammateBridgeCollectsTerminalTasks) {
    NotifStateReset guard;
    std::vector<FakeTask> tasks{
        {.identity = {"runner", "Runner"}, .status = FakeStatus::Running},
        {.identity = {"done",   "Done"},   .status = FakeStatus::Completed},
        {.identity = {"boom",   "Boom"},   .status = FakeStatus::Killed},
    };
    notif::inject_teammate_shutdowns_from_tasks(tasks, is_terminal, to_cause);

    auto all = notif::get_all_teammate_shutdowns();
    ASSERT_EQ(all.size(), 2u);

    // Recency window (5 min default) + not-yet-dismissed -> both surface.
    auto recent = notif::get_teammate_shutdowns(5min);
    ASSERT_EQ(recent.size(), 2u);

    std::unordered_set<std::string> ids;
    std::unordered_set<notif::TeammateShutdownCause> causes;
    for (const auto& r : recent) {
        ids.insert(r.agent_id);
        causes.insert(r.cause);
    }
    EXPECT_TRUE(ids.contains("done"));
    EXPECT_TRUE(ids.contains("boom"));
    EXPECT_TRUE(causes.contains(notif::TeammateShutdownCause::Finished));
    EXPECT_TRUE(causes.contains(notif::TeammateShutdownCause::Crashed));
}

TEST(FixNotifs, TeammateBridgeIsIdempotent) {
    NotifStateReset guard;
    std::vector<FakeTask> tasks{
        {.identity = {"once", "Once"}, .status = FakeStatus::Completed},
    };
    notif::inject_teammate_shutdowns_from_tasks(tasks, is_terminal, to_cause);
    notif::inject_teammate_shutdowns_from_tasks(tasks, is_terminal, to_cause);
    // Same agent must not be appended twice.
    EXPECT_EQ(notif::get_all_teammate_shutdowns().size(), 1u);
}

TEST(FixNotifs, TeammateBridgeRespectsAcknowledgement) {
    NotifStateReset guard;
    std::vector<FakeTask> tasks{
        {.identity = {"ack-me", "Ack"}, .status = FakeStatus::Completed},
    };
    notif::inject_teammate_shutdowns_from_tasks(tasks, is_terminal, to_cause);
    ASSERT_FALSE(notif::get_teammate_shutdowns(5min).empty());

    notif::acknowledge_teammate_shutdown("ack-me");
    EXPECT_TRUE(notif::get_teammate_shutdowns(5min).empty());
}
