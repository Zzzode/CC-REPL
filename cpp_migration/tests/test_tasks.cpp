/// @file test_tasks.cpp
/// @brief cc_tasks migration parity tests.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

import cc.tools.agent_runtime;
import cc.tools.agent_types;
import cc.tools.spawn_multi_agent;
import cc.tools.team;
import cc.tasks.local_agent_task;
import cc.tasks.in_process_teammate_task;
import cc.tasks.pill_label;
import cc.tasks.task;
import cc.tasks.types;
import cc.utils.swarm_backends;
import cc.utils.swarm_pane_observer;

namespace fs = std::filesystem;

namespace {

struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvironmentGuard(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

[[nodiscard]] fs::path unique_test_dir(std::string_view prefix) {
    return fs::temp_directory_path() / (
        std::string(prefix) +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
    );
}

} // namespace

TEST(SpawnMultiAgent, TeamNameSpawnsTeammateBackend) {
    const auto root = unique_test_dir("cc-repl-spawn-multi-agent-");
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    EnvironmentGuard backend_guard("CC_REPL_TEAMMATE_BACKEND", "in-process");
    EnvironmentGuard verification_guard("CLAUDE_CODE_ENABLE_VERIFICATION_AGENT", "1");
    cc::utils::swarm_backends::BackendRegistry::reset();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    auto created = cc::tools::global_team_store().create(
        "migration-team",
        "migration-team",
        std::vector<cc::tools::TeamMember>{
            cc::tools::TeamMember{
                .agent_id = "reviewer-two@migration-team",
                .role = cc::tools::MemberRole::Reviewer,
                .status = cc::tools::MemberStatus::Working,
                .current_task = std::nullopt,
                .last_result = std::nullopt,
            }
        }
    );
    ASSERT_TRUE(created);

    cc::tools::MultiAgentConfig config{
        .agents = {
            cc::tools::MultiAgentAgentConfig{
                .type = cc::tools::AgentType::Verify,
                .name = "reviewer-two",
                .model = "haiku",
                .system_prompt = "Review migration parity",
                .allowed_tools = {"Read"},
                .max_turns = 1,
            },
        },
        .parallel = false,
        .coordinator_prompt = std::string("Review the team migration"),
        .team_name = std::string("migration-team"),
        .working_dir = root.string(),
        .prefer_in_process = true,
    };

    auto futures = cc::tools::spawn_agents(config);
    auto results = cc::tools::wait_all(
        std::span<std::future<cc::tools::MultiAgentResult>>(futures.data(), futures.size())
    );

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].completed) << results[0].output;
    EXPECT_NE(results[0].output.find("Spawned successfully."), std::string::npos) << results[0].output;
    EXPECT_NE(results[0].output.find("agent_id: reviewer-two-2@migration-team"), std::string::npos) << results[0].output;
    EXPECT_NE(results[0].output.find("backend: in-process"), std::string::npos) << results[0].output;
    EXPECT_NE(results[0].output.find("status: teammate_spawned"), std::string::npos) << results[0].output;
    EXPECT_EQ(results[0].output.find("Agent 'reviewer-two'"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("reviewer-two-2@migration-team");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    EXPECT_TRUE(record->background);
    ASSERT_TRUE(record->teammate_backend);
    EXPECT_EQ(*record->teammate_backend, "in-process");
    ASSERT_TRUE(record->teammate_task_id);
    EXPECT_EQ(*record->teammate_task_id, "in-process:reviewer-two-2@migration-team");
    ASSERT_TRUE(record->name);
    EXPECT_EQ(*record->name, "reviewer-two-2");
    ASSERT_TRUE(record->team_name);
    EXPECT_EQ(*record->team_name, "migration-team");
    ASSERT_TRUE(record->cwd);
    EXPECT_EQ(*record->cwd, fs::weakly_canonical(root).string());
    ASSERT_GE(record->transcript.size(), 1u);
    EXPECT_EQ(record->transcript[0], "user: Review the team migration");

    auto team = cc::tools::global_team_store().get_by_id_or_name("migration-team");
    ASSERT_TRUE(team);
    ASSERT_EQ((*team)->members.size(), 2u);
    auto member = std::ranges::find_if((*team)->members, [](const auto& candidate) {
        return candidate.agent_id == "reviewer-two-2@migration-team";
    });
    ASSERT_NE(member, (*team)->members.end());
    EXPECT_EQ(member->role, cc::tools::MemberRole::Reviewer);
    EXPECT_EQ(member->status, cc::tools::MemberStatus::Working);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    cc::tools::global_team_store().clear_for_testing();
    cc::utils::swarm_backends::BackendRegistry::reset();
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(SpawnMultiAgent, NonTeamAgentsUseAgentToolBackgroundPath) {
    const auto root = unique_test_dir("cc-repl-spawn-multi-agent-local-");
    fs::create_directories(root);
    EnvironmentGuard agent_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::MultiAgentConfig config{
        .agents = {
            cc::tools::MultiAgentAgentConfig{
                .type = cc::tools::AgentType::GeneralPurpose,
                .name = "local-worker",
                .model = "haiku",
                .system_prompt = "Review migration parity",
                .allowed_tools = {"Read"},
                .max_turns = 1,
            },
        },
        .parallel = false,
        .coordinator_prompt = std::string("Inspect the non-team migration path"),
        .team_name = std::nullopt,
        .working_dir = root.string(),
        .prefer_in_process = true,
    };

    auto futures = cc::tools::spawn_agents(config);
    auto results = cc::tools::wait_all(
        std::span<std::future<cc::tools::MultiAgentResult>>(futures.data(), futures.size())
    );

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].completed) << results[0].output;
    EXPECT_NE(results[0].output.find("Queued background agent local-worker"), std::string::npos) << results[0].output;
    EXPECT_EQ(results[0].output.find("Agent 'local-worker'"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("local-worker");
    ASSERT_TRUE(record);
    EXPECT_TRUE(record->background);
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    EXPECT_EQ(record->agent_type, "general-purpose");
    ASSERT_TRUE(record->cwd);
    EXPECT_EQ(*record->cwd, fs::weakly_canonical(root).string());
    ASSERT_FALSE(record->transcript.empty());
    EXPECT_NE(record->transcript.front().find("Inspect the non-team migration path"), std::string::npos);

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(LocalAgentTask, NotificationLeavesOutputFileEmptyWhenNoPathIsKnown) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "completed"
    );

    EXPECT_NE(xml.find("<task_id>task_123</task_id>"), std::string::npos);
    EXPECT_NE(xml.find("<output_file></output_file>"), std::string::npos);
    EXPECT_EQ(xml.find("<output_file>task_123</output_file>"), std::string::npos);
}

TEST(LocalAgentTask, NotificationUsesExplicitOutputFileWhenProvided) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "completed",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        "/tmp/agent-output.log"
    );

    EXPECT_NE(xml.find("<output_file>/tmp/agent-output.log</output_file>"), std::string::npos);
}

TEST(LocalAgentTask, NotificationEscapesXmlTextNodes) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_<1>&",
        "Review <code>&docs",
        "failed",
        "bad <tag>&value",
        "result </result> & more",
        "tool_<id>",
        "/tmp/a&b",
        "branch<main>",
        "/tmp/out&1.log"
    );

    EXPECT_NE(xml.find("task_&lt;1&gt;&amp;"), std::string::npos);
    EXPECT_NE(xml.find("Review &lt;code&gt;&amp;docs"), std::string::npos);
    EXPECT_NE(xml.find("bad &lt;tag&gt;&amp;value"), std::string::npos);
    EXPECT_NE(xml.find("result &lt;/result&gt; &amp; more"), std::string::npos);
    EXPECT_NE(xml.find("tool_&lt;id&gt;"), std::string::npos);
    EXPECT_NE(xml.find("/tmp/a&amp;b"), std::string::npos);
    EXPECT_NE(xml.find("branch&lt;main&gt;"), std::string::npos);
    EXPECT_NE(xml.find("/tmp/out&amp;1.log"), std::string::npos);
}

TEST(LocalAgentTask, StoppedNotificationUsesStoppedStatusAndSummary) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "stopped"
    );

    EXPECT_NE(xml.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(xml.find("Agent \"Review code\" was stopped"), std::string::npos);
    EXPECT_EQ(xml.find("killed"), std::string::npos);
}

TEST(InProcessTeammateTask, AppendMessageStoresUiVisiblePendingMessage) {
    cc::tasks::InProcessTeammateTaskState state{};
    state.type = cc::core::TaskType::InProcessTeammate;
    state.status = cc::core::TaskStatus::Running;

    cc::tasks::append_teammate_message("tm_1", "hello", [&](const std::string&, std::function<void(cc::tasks::InProcessTeammateTaskState&)> mutate) {
        mutate(state);
    });

    ASSERT_EQ(state.pending_user_messages.size(), 1u);
    EXPECT_EQ(state.pending_user_messages[0], "hello");
}

TEST(InProcessTeammateTask, FiltersTaskRegistryToTeammateStates) {
    cc::tasks::InProcessTeammateTaskState teammate{};
    teammate.type = cc::core::TaskType::InProcessTeammate;
    teammate.status = cc::core::TaskStatus::Running;
    teammate.identity.agent_id = "researcher@team";
    teammate.identity.agent_name = "researcher";

    cc::tasks::LocalAgentTaskState local_agent{};
    local_agent.type = cc::core::TaskType::LocalAgent;
    local_agent.status = cc::core::TaskStatus::Running;

    std::vector<cc::core::TaskStateBase*> registry = {&teammate, &local_agent};
    auto result = cc::tasks::get_all_in_process_teammate_tasks(registry);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].identity.agent_id, "researcher@team");
}

TEST(PillLabel, CountsShellsAndMonitorsSeparately) {
    cc::tasks::LocalShellTaskState shell{};
    shell.type = cc::core::TaskType::LocalBash;
    shell.kind = cc::tasks::BashTaskKind::Bash;

    cc::tasks::LocalShellTaskState monitor{};
    monitor.type = cc::core::TaskType::LocalBash;
    monitor.kind = cc::tasks::BashTaskKind::Monitor;

    std::vector<cc::core::TaskStateBase*> tasks = {&shell, &monitor};

    EXPECT_EQ(cc::tasks::get_pill_label(tasks), "1 shell, 1 monitor");
}

TEST(PillLabel, CountsUniqueTeammateTeams) {
    cc::tasks::InProcessTeammateTaskState researcher{};
    researcher.type = cc::core::TaskType::InProcessTeammate;
    researcher.identity.team_name = "alpha";

    cc::tasks::InProcessTeammateTaskState reviewer{};
    reviewer.type = cc::core::TaskType::InProcessTeammate;
    reviewer.identity.team_name = "alpha";

    cc::tasks::InProcessTeammateTaskState implementer{};
    implementer.type = cc::core::TaskType::InProcessTeammate;
    implementer.identity.team_name = "beta";

    std::vector<cc::core::TaskStateBase*> tasks = {&researcher, &reviewer, &implementer};

    EXPECT_EQ(cc::tasks::get_pill_label(tasks), "2 teams");
}

TEST(PillLabel, ShowsUltraplanAttentionStatesAndCta) {
    cc::tasks::RemoteAgentTaskState remote{};
    remote.type = cc::core::TaskType::RemoteAgent;
    remote.is_ultraplan = true;

    std::vector<cc::core::TaskStateBase*> tasks = {&remote};

    EXPECT_EQ(
        cc::tasks::get_pill_label(tasks),
        std::string(cc::tasks::DIAMOND_OPEN) + " ultraplan");
    EXPECT_FALSE(cc::tasks::pill_needs_cta(tasks));

    remote.ultraplan_phase = cc::tasks::UltraplanPhase::NeedsInput;
    EXPECT_EQ(
        cc::tasks::get_pill_label(tasks),
        std::string(cc::tasks::DIAMOND_OPEN) + " ultraplan needs your input");
    EXPECT_TRUE(cc::tasks::pill_needs_cta(tasks));

    remote.ultraplan_phase = cc::tasks::UltraplanPhase::PlanReady;
    EXPECT_EQ(
        cc::tasks::get_pill_label(tasks),
        std::string(cc::tasks::DIAMOND_FILLED) + " ultraplan ready");
    EXPECT_TRUE(cc::tasks::pill_needs_cta(tasks));
}

TEST(PillLabel, ShowsRemoteCloudSessionLabelForNonUltraplanTasks) {
    cc::tasks::RemoteAgentTaskState remote{};
    remote.type = cc::core::TaskType::RemoteAgent;

    std::vector<cc::core::TaskStateBase*> tasks = {&remote};

    EXPECT_EQ(
        cc::tasks::get_pill_label(tasks),
        std::string(cc::tasks::DIAMOND_OPEN) + " 1 cloud session");
    EXPECT_FALSE(cc::tasks::pill_needs_cta(tasks));
}

// ============================================================================
// Stage B: pane capture + leader-side pane observer
// ============================================================================

namespace cc_repl_pane_observer_test {

namespace sw = cc::utils::swarm_backends;
namespace po = cc::utils::pane_observer;

// In-memory pane backend that scripts a queue of capture results per pane,
// so observer polling/done-detection can be driven without tmux.
class ScriptedPaneBackend : public sw::PaneBackend {
public:
    int create_calls = 0;
    bool available = true;
    bool inside = false;
    std::map<sw::PaneId, std::deque<std::optional<std::string>>> scripted_captures;
    std::vector<std::pair<sw::PaneId, int>> capture_calls;

    sw::BackendType type() const override { return sw::BackendType::Tmux; }
    std::string_view display_name() const override { return "scripted-tmux"; }
    bool supports_hide_show() const override { return false; }
    bool is_available() const override { return available; }
    bool is_running_inside() const override { return inside; }

    sw::CreatePaneResult create_teammate_pane(std::string_view, sw::AgentColor) override {
        ++create_calls;
        return sw::CreatePaneResult{
            .pane_id = sw::PaneId{"%p" + std::to_string(create_calls)},
            .is_first_teammate = (create_calls == 1)};
    }
    void send_command_to_pane(const sw::PaneId&, std::string_view, bool) override {}
    void set_pane_border_color(const sw::PaneId&, sw::AgentColor, bool) override {}
    void set_pane_title(const sw::PaneId&, std::string_view, sw::AgentColor, bool) override {}
    void enable_pane_border_status(std::optional<std::string_view>, bool) override {}
    void rebalance_panes(std::string_view, bool) override {}
    bool kill_pane(const sw::PaneId&, bool) override { return true; }
    bool hide_pane(const sw::PaneId&, bool) override { return false; }
    bool show_pane(const sw::PaneId&, std::string_view, bool) override { return false; }

    std::optional<std::string> capture_pane_text(
        const sw::PaneId& id, int tail_lines, bool
    ) override {
        capture_calls.emplace_back(id, tail_lines);
        auto it = scripted_captures.find(id);
        if (it == scripted_captures.end() || it->second.empty()) return std::nullopt;
        auto next = std::move(it->second.front());
        it->second.pop_front();
        return next;
    }
};

TEST(SwarmBackends, CapturePaneArgBuilder) {
    EXPECT_EQ(
        sw::detail::build_capture_pane_command("%42", 200),
        "tmux capture-pane -p -t '%42' -S -200 2>/dev/null");
    EXPECT_EQ(
        sw::detail::build_capture_pane_command("%7", 1),
        "tmux capture-pane -p -t '%7' -S -1 2>/dev/null");
    // tail_lines below 1 clamps to 1.
    EXPECT_EQ(
        sw::detail::build_capture_pane_command("%7", 0),
        "tmux capture-pane -p -t '%7' -S -1 2>/dev/null");
    EXPECT_EQ(
        sw::detail::build_capture_pane_command("%7", -50),
        "tmux capture-pane -p -t '%7' -S -1 2>/dev/null");
}

TEST(SwarmBackends, ExecutorCapturesByAgent) {
    auto fake = std::make_shared<ScriptedPaneBackend>();
    fake->scripted_captures["%p1"].emplace_back(std::string("line1\nline2"));

    sw::PaneBackendExecutor exec(fake);
    // Empty prompt keeps this hermetic (no mailbox runtime dir required).
    sw::TeammateSpawnConfig cfg{};
    cfg.name = "worker";
    cfg.team_name = "t1";
    ASSERT_TRUE(exec.spawn(cfg).success);

    EXPECT_EQ(
        exec.capture_agent_pane("worker@t1"),
        std::optional<std::string>("line1\nline2"));
    EXPECT_EQ(exec.capture_agent_pane("ghost@t1"), std::nullopt);

    auto panes = exec.spawned_panes();
    ASSERT_EQ(panes.size(), 1u);
    EXPECT_EQ(panes.begin()->first, "worker@t1");
    EXPECT_EQ(panes.begin()->second.pane_id, "%p1");
    EXPECT_FALSE(panes.begin()->second.inside_tmux);

    // Default tail lines plumbed through to the backend capture call.
    ASSERT_FALSE(fake->capture_calls.empty());
    EXPECT_EQ(fake->capture_calls.back().first, "%p1");
    EXPECT_EQ(fake->capture_calls.back().second, 200);
}

TEST(PaneObserver, DedupesAndTracksPerAgent) {
    auto fake = std::make_shared<ScriptedPaneBackend>();
    po::PaneObserver obs(fake, std::chrono::milliseconds(1500), 5, /*auto_start=*/false);

    obs.track("a@t", "%p1");
    obs.track("b@t", "%p2");
    EXPECT_EQ(obs.tracked_agent_ids().size(), 2u);

    // a: same screen twice, then a changed screen. b: two failures.
    fake->scripted_captures["%p1"].emplace_back(std::string("line1\nline2"));
    fake->scripted_captures["%p1"].emplace_back(std::string("line1\nline2"));
    fake->scripted_captures["%p1"].emplace_back(std::string("x\ny\nz"));
    fake->scripted_captures["%p2"].push_back(std::nullopt);
    fake->scripted_captures["%p2"].push_back(std::nullopt);

    obs.refresh_once();
    // First pass: a gets revision 1; b has one failure but is still Running.
    {
        auto a = obs.get_agent_snapshot("a@t");
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->revision, 1u);
        EXPECT_EQ(a->state, po::PaneRunState::Running);
        ASSERT_EQ(a->lines.size(), 2u);
        EXPECT_EQ(a->lines[0], "line1");
        EXPECT_EQ(a->lines[1], "line2");
        auto b = obs.get_agent_snapshot("b@t");
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(b->consecutive_failures, 1);
        EXPECT_EQ(b->state, po::PaneRunState::Running);
    }

    obs.refresh_once();
    // Identical capture must not bump a's revision; b's second failure flips
    // it to Done (kFailureThreshold == 2).
    {
        auto a = obs.get_agent_snapshot("a@t");
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->revision, 1u);
        auto b = obs.get_agent_snapshot("b@t");
        ASSERT_TRUE(b.has_value());
        EXPECT_EQ(b->state, po::PaneRunState::Done);
    }

    obs.refresh_once();
    // Changed capture bumps revision and re-splits lines; a stays Running.
    {
        auto a = obs.get_agent_snapshot("a@t");
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->revision, 2u);
        EXPECT_EQ(a->state, po::PaneRunState::Running);
        ASSERT_EQ(a->lines.size(), 3u);
        EXPECT_EQ(a->lines[2], "z");
    }

    // Deep-copy snapshot keyed by agent_id.
    auto snapshot = obs.get_pane_snapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    ASSERT_TRUE(snapshot.contains("a@t"));
    ASSERT_TRUE(snapshot.contains("b@t"));

    obs.untrack("b@t");
    EXPECT_EQ(obs.tracked_agent_ids().size(), 1u);
    EXPECT_EQ(obs.get_agent_snapshot("b@t"), std::nullopt);
    EXPECT_EQ(obs.get_pane_snapshot().size(), 1u);
}

TEST(PaneObserver, GlobalSingletonLifecycle) {
    // Start clean regardless of any earlier test in this binary.
    po::shutdown_global_pane_observer();
    EXPECT_EQ(po::global_pane_observer(), nullptr);

    auto fake = std::make_shared<ScriptedPaneBackend>();
    auto o1 = po::ensure_global_pane_observer(fake);
    auto o2 = po::ensure_global_pane_observer(fake);
    ASSERT_TRUE(o1 != nullptr);
    EXPECT_EQ(o1, o2);
    EXPECT_EQ(po::global_pane_observer(), o1);

    po::shutdown_global_pane_observer();
    EXPECT_EQ(po::global_pane_observer(), nullptr);
}

} // namespace cc_repl_pane_observer_test
