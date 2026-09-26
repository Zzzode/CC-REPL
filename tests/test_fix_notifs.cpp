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

import std;
import cc.hooks.remaining_notifs;
import cc.services.mcp.connection_manager;
import cc.services.mcp.types;
import cc.bootstrap.mcp_connectivity;
import cc.tools.mcp;

namespace notif = cc::hooks::notifs;
namespace svc_mcp = cc::services::mcp;
namespace bridge = cc::bootstrap::mcp_connectivity;

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

// RFC-0001 B7: wire_mcp_connectivity() installs into the process-global B6
// snapshot sink. Reset it (and sync the runtime back to zero servers, and
// clear the hook slot) so later cases never see the bridge lambda.
struct BridgeWireGuard {
    ~BridgeWireGuard() {
        cc::tools::set_mcp_snapshots_sink(nullptr);
        (void)cc::tools::sync_native_mcp_servers({});
        notif::clear_mcp_connectivity();
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

// RFC-0001 B7: the mapping cases assert BOTH the hook-local mapper (still
// present this batch) and the new bootstrap bridge mapper yield identical
// results. B8 deletes the hook-local copy, leaving the bridge assertions.

TEST(FixNotifs, McpStatusMappingConnected) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Connected),
              notif::McpServerStatus::Connected);
    EXPECT_EQ(bridge::to_hook_status(svc_mcp::ConnectionStatus::Connected),
              notif::McpServerStatus::Connected);
}

TEST(FixNotifs, McpStatusMappingConnecting) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Connecting),
              notif::McpServerStatus::Connecting);
    EXPECT_EQ(bridge::to_hook_status(svc_mcp::ConnectionStatus::Connecting),
              notif::McpServerStatus::Connecting);
}

TEST(FixNotifs, McpStatusMappingDisconnected) {
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Disconnected),
              notif::McpServerStatus::Disconnected);
    EXPECT_EQ(bridge::to_hook_status(svc_mcp::ConnectionStatus::Disconnected),
              notif::McpServerStatus::Disconnected);
}

TEST(FixNotifs, McpStatusMappingErrorAndNeedsAuth) {
    // Both Error and NeedsAuth project onto the surfacing Error bucket so the
    // existing has_mcp_connectivity_issues() reader flags them.
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::Error),
              notif::McpServerStatus::Error);
    EXPECT_EQ(notif::to_mcp_server_status(svc_mcp::ConnectionStatus::NeedsAuth),
              notif::McpServerStatus::Error);
    EXPECT_EQ(bridge::to_hook_status(svc_mcp::ConnectionStatus::Error),
              notif::McpServerStatus::Error);
    EXPECT_EQ(bridge::to_hook_status(svc_mcp::ConnectionStatus::NeedsAuth),
              notif::McpServerStatus::Error);
}

// ─── RFC-0001 B7 bootstrap bridge ───────────────────────────────────────────

TEST(FixNotifs, ProjectConnectivityMapsSnapshots) {
    NotifStateReset guard;

    std::vector<svc_mcp::McpServerSnapshot> snaps;
    svc_mcp::McpServerSnapshot healthy;
    healthy.name = "snap_alpha";
    healthy.status = svc_mcp::ConnectionStatus::Connected;
    snaps.push_back(std::move(healthy));
    svc_mcp::McpServerSnapshot authy;
    authy.name = "snap_beta";
    authy.status = svc_mcp::ConnectionStatus::NeedsAuth;
    authy.last_error = "needs auth";
    snaps.push_back(std::move(authy));

    const int64_t before = notif::detail::now_ms();
    auto infos = bridge::project_connectivity(std::move(snaps));
    const int64_t after = notif::detail::now_ms();

    ASSERT_EQ(infos.size(), 2u);

    // Both ids copy the snapshot name; order is preserved.
    EXPECT_EQ(infos[0].server_id, "snap_alpha");
    EXPECT_EQ(infos[0].display_name, "snap_alpha");
    EXPECT_EQ(infos[0].state, notif::McpServerStatus::Connected);
    EXPECT_FALSE(infos[0].last_error.has_value());

    // NeedsAuth surfaces as Error and last_error is copied verbatim.
    EXPECT_EQ(infos[1].server_id, "snap_beta");
    EXPECT_EQ(infos[1].display_name, "snap_beta");
    EXPECT_EQ(infos[1].state, notif::McpServerStatus::Error);
    ASSERT_TRUE(infos[1].last_error.has_value());
    EXPECT_EQ(*infos[1].last_error, "needs auth");

    // One shared now-ms stamp per projection call, sourced from the hook
    // clock; positive and within the bracketed wall-clock window.
    EXPECT_GT(infos[0].last_seen_ms, 0);
    EXPECT_EQ(infos[0].last_seen_ms, infos[1].last_seen_ms);
    EXPECT_GE(infos[0].last_seen_ms, before);
    EXPECT_LE(infos[0].last_seen_ms, after);
}

// Installs the bridge sink at the composition seam, then drives a runtime
// status read with ZERO configured servers (sync marks the runtime loaded,
// bypassing config discovery entirely, so no detached connect threads and
// the manager iterates an empty mcp_config_.servers deterministically). The
// sink must replace the pre-seeded sentinel slot content with {}.
//
// INTERIM B7: the old in-runtime projection publishes {} on the same read
// too (identical double-publish); post-B8 this same test proves the sink is
// the sole feed.
TEST(FixNotifs, WireSinkRefreshesSlot) {
    NotifStateReset state_guard;
    BridgeWireGuard sink_guard;

    notif::set_raw_mcp_connectivity({
        notif::McpConnectivityInfo{
            .server_id = "sentinel",
            .display_name = "Sentinel",
            .state = notif::McpServerStatus::Error,
            .last_error = std::string("sentinel"),
            .last_seen_ms = 123,
        },
    });
    ASSERT_EQ(notif::get_mcp_connectivity_status().size(), 1u);

    bridge::wire_mcp_connectivity();
    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());

    auto statuses = cc::tools::native_mcp_statuses();
    EXPECT_TRUE(statuses.empty());
    EXPECT_TRUE(notif::get_mcp_connectivity_status().empty());
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
