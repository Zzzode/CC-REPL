/// @file test_state.cpp


/// persistence, on_change_app_state, ftxui_integration

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <memory>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

import cc.state.app_state;
import cc.state.store;
import cc.state.selectors;
import cc.state.persistence;
import cc.state.on_change;
import cc.state.ftxui_integration;
import cc.session.history;
import cc.types.types;
import cc.cli.update;
import cc.services.mcp.auth;
import cc.services.mcp.types;
import cc.utils.error;
import cc.constants.prompts;

namespace {

[[nodiscard]] std::unique_ptr<cc::state::AppStore> make_test_store() {
    return std::make_unique<cc::state::AppStore>(
        cc::state::get_default_app_state(),
        &cc::state::app_reducer);
}

[[nodiscard]] std::shared_ptr<cc::state::AppStore> make_shared_test_store() {
    return std::make_shared<cc::state::AppStore>(
        cc::state::get_default_app_state(),
        &cc::state::app_reducer);
}

[[nodiscard]] cc::core::Message make_user_message(std::string id, std::string text) {
    return cc::core::UserMessage{
        cc::core::MessageBase{
            cc::core::MessageId{std::move(id)},
            std::chrono::system_clock::now(),
            {cc::core::TextBlock{std::move(text)}}
        }
    };
}

} // namespace
// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(AppState, DefaultStateIsValid) {
    auto state = cc::state::get_default_app_state();
    EXPECT_LE(state.created_at, std::chrono::system_clock::now());
    EXPECT_FALSE(state.verbose);
    EXPECT_FALSE(state.is_loading);
    EXPECT_FALSE(state.is_streaming);
}

TEST(AppState, ObservableState) {
    cc::state::ObservableState obs_state;
    int change_count = 0;
    
    auto sub_id = obs_state.subscribe([&change_count](const auto&, const auto&) {
        change_count++;
    });
    EXPECT_NE(sub_id, 0u);
    
    auto new_state = cc::state::get_default_app_state();
    new_state.verbose = true;
    obs_state.set(new_state);
    
    EXPECT_EQ(change_count, 1);
    EXPECT_TRUE(obs_state.get().verbose);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(StateStore, InitialState) {
    auto store = make_test_store();
    auto state = store->get_state();

    EXPECT_LE(state.created_at, std::chrono::system_clock::now());
}

TEST(StateStore, DispatchSetVerbose) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});

    auto state = store->get_state();
    EXPECT_TRUE(state.verbose);
}

TEST(StateStore, DispatchSetLoading) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});

    auto state = store->get_state();
    EXPECT_TRUE(state.is_loading);
}

TEST(StateStore, SubscribeReceivesNotifications) {
    auto store = make_test_store();
    int notify_count = 0;


    auto sub_id = store->subscribe([&notify_count](const auto& /*prev*/, const auto& /*next*/) {
        notify_count++;
    });

    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});

    EXPECT_EQ(notify_count, 2);


    store->unsubscribe(sub_id);
    store->dispatch(cc::state::Action{cc::state::ActionType::SetStreaming, true});
    EXPECT_EQ(notify_count, 2);
}

TEST(StateStore, UnknownActionNoOp) {
    auto store = make_test_store();
    auto before = store->get_state();


    store->dispatch(cc::state::Action{cc::state::ActionType::EnableTool});
    auto after = store->get_state();

    EXPECT_EQ(before.verbose, after.verbose);
    EXPECT_EQ(before.is_loading, after.is_loading);
}

TEST(StateStore, DispatchPermissionParityActions) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::GrantPermission, std::string{"Bash"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::RevokePermission, std::string{"Bash"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::GrantPermission, std::string{"Read"}});

    auto state = store->get_state();
    EXPECT_FALSE(state.tool_permission_context.allowed_tools.contains("Bash"));
    EXPECT_TRUE(state.tool_permission_context.denied_tools.contains("Bash"));
    EXPECT_TRUE(state.tool_permission_context.allowed_tools.contains("Read"));
    EXPECT_FALSE(state.tool_permission_context.denied_tools.contains("Read"));
}

TEST(StateStore, DispatchMessageAndSettingsParityActions) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{
        cc::state::ActionType::AddMessage,
        make_user_message("msg-1", "first")});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::UpdateLastMessage,
        make_user_message("msg-2", "replacement")});

    cc::state::Settings settings;
    settings.model = "claude-sonnet-4-6";
    settings.theme = "dark";
    settings.verbose = true;
    store->dispatch(cc::state::Action{cc::state::ActionType::UpdateSettings, settings});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetThinkingEnabled, false});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetPromptSuggestionEnabled, false});

    auto state = store->get_state();
    ASSERT_EQ(state.messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::UserMessage>(state.messages.front()));
    const auto& updated = std::get<cc::core::UserMessage>(state.messages.front());
    EXPECT_EQ(updated.id.value, "msg-2");
    ASSERT_EQ(updated.content.size(), 1u);
    EXPECT_EQ(std::get<cc::core::TextBlock>(updated.content.front()).text, "replacement");
    EXPECT_EQ(state.settings.model, "claude-sonnet-4-6");
    EXPECT_EQ(state.settings.theme, "dark");
    EXPECT_TRUE(state.settings.verbose);
    EXPECT_FALSE(state.thinking_enabled);
    EXPECT_FALSE(state.prompt_suggestion_enabled);
}

TEST(StateStore, DispatchUiParityActions) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetSlashCommand,
        std::optional<std::string>{"/help"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::AddNotification, std::string{"n1"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::AddNotification, std::string{"n2"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::DismissNotification, std::string{"n1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetStatusLineText,
        std::optional<std::string>{"ready"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetFooterSelection,
        std::optional<cc::state::FooterItem>{cc::state::FooterItem::Tasks}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetSpinnerTip,
        std::optional<std::string>{"working"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBriefOnly, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetShowTeammatePreview, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetSelectedAgentIndex, 2});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetCoordinatorTaskIndex, 3});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetViewSelectionMode, std::string{"viewing-agent"}});

    auto state = store->get_state();
    ASSERT_TRUE(state.active_slash_command.has_value());
    EXPECT_EQ(*state.active_slash_command, "/help");
    ASSERT_EQ(state.notifications.size(), 1u);
    EXPECT_EQ(state.notifications.front(), "n2");
    ASSERT_TRUE(state.status_line_text.has_value());
    EXPECT_EQ(*state.status_line_text, "ready");
    ASSERT_TRUE(state.footer_selection.has_value());
    EXPECT_EQ(*state.footer_selection, cc::state::FooterItem::Tasks);
    ASSERT_TRUE(state.spinner_tip.has_value());
    EXPECT_EQ(*state.spinner_tip, "working");
    EXPECT_TRUE(state.is_brief_only);
    EXPECT_TRUE(state.show_teammate_message_preview);
    EXPECT_EQ(state.selected_ip_agent_index, 2);
    EXPECT_EQ(state.coordinator_task_index, 3);
    EXPECT_EQ(state.view_selection_mode, "viewing-agent");
}

TEST(StateStore, DispatchBridgeAndRemoteParityActions) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeEnabled, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeExplicit, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeOutboundOnly, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeConnected, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeSessionActive, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetBridgeReconnecting, true});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeConnectUrl,
        std::optional<std::string>{"http://bridge/connect"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeSessionUrl,
        std::optional<std::string>{"http://bridge/session"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeEnvironmentId,
        std::optional<std::string>{"env-1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeSessionId,
        std::optional<std::string>{"session-1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeError,
        std::optional<std::string>{"bridge error"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetBridgeInitialName,
        std::optional<std::string>{"local"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetShowRemoteCallout, true});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetRemoteSessionUrl,
        std::optional<std::string>{"http://remote/session"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetRemoteConnectionStatus,
        cc::state::RemoteConnectionStatus::Connected});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetRemoteBackgroundTaskCount, 7u});

    auto state = store->get_state();
    EXPECT_TRUE(state.repl_bridge_enabled);
    EXPECT_TRUE(state.repl_bridge_explicit);
    EXPECT_TRUE(state.repl_bridge_outbound_only);
    EXPECT_TRUE(state.repl_bridge_connected);
    EXPECT_TRUE(state.repl_bridge_session_active);
    EXPECT_TRUE(state.repl_bridge_reconnecting);
    EXPECT_EQ(state.repl_bridge_connect_url, std::optional<std::string>{"http://bridge/connect"});
    EXPECT_EQ(state.repl_bridge_session_url, std::optional<std::string>{"http://bridge/session"});
    EXPECT_EQ(state.repl_bridge_environment_id, std::optional<std::string>{"env-1"});
    EXPECT_EQ(state.repl_bridge_session_id, std::optional<std::string>{"session-1"});
    EXPECT_EQ(state.repl_bridge_error, std::optional<std::string>{"bridge error"});
    EXPECT_EQ(state.repl_bridge_initial_name, std::optional<std::string>{"local"});
    EXPECT_TRUE(state.show_remote_callout);
    EXPECT_EQ(state.remote_session_url, std::optional<std::string>{"http://remote/session"});
    EXPECT_EQ(state.remote_connection_status, cc::state::RemoteConnectionStatus::Connected);
    EXPECT_EQ(state.remote_background_task_count, 7u);
}

TEST(StateStore, DispatchTasksAgentsAndOverlayParityActions) {
    auto store = make_test_store();

    cc::state::TaskState task{
        .id = "task-1",
        .title = "Investigate",
        .status = "running",
        .messages = {},
        .created_at = std::chrono::system_clock::now(),
    };
    store->dispatch(cc::state::Action{cc::state::ActionType::AddTask, task});
    task.status = "done";
    store->dispatch(cc::state::Action{cc::state::ActionType::UpdateTask, task});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetForegroundedTaskId,
        std::optional<std::string>{"task-1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetViewingAgentTaskId,
        std::optional<std::string>{"task-1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::RegisterAgentName,
        std::pair<std::string, std::string>{"agent-a", "task-1"}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetAgent,
        std::optional<std::string>{"agent-a"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetKairosEnabled, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetCompanionReaction, std::optional<std::string>{"ok"}});
    const auto pet_time = std::chrono::system_clock::now();
    store->dispatch(cc::state::Action{cc::state::ActionType::SetCompanionPetTime, std::optional{pet_time}});
    store->dispatch(cc::state::Action{cc::state::ActionType::AddActiveOverlay, std::string{"help"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::RemoveActiveOverlay, std::string{"help"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::AddActiveOverlay, std::string{"tasks"}});

    auto state = store->get_state();
    ASSERT_TRUE(state.tasks.contains("task-1"));
    EXPECT_EQ(state.tasks.at("task-1").status, "done");
    EXPECT_EQ(state.foregrounded_task_id, std::optional<std::string>{"task-1"});
    EXPECT_EQ(state.viewing_agent_task_id, std::optional<std::string>{"task-1"});
    ASSERT_TRUE(state.agent_name_registry.contains("agent-a"));
    EXPECT_EQ(state.agent_name_registry.at("agent-a"), "task-1");
    EXPECT_EQ(state.agent, std::optional<std::string>{"agent-a"});
    EXPECT_TRUE(state.kairos_enabled);
    EXPECT_EQ(state.companion_reaction, std::optional<std::string>{"ok"});
    EXPECT_EQ(state.companion_pet_at, std::optional{pet_time});
    EXPECT_FALSE(state.active_overlays.contains("help"));
    EXPECT_TRUE(state.active_overlays.contains("tasks"));

    store->dispatch(cc::state::Action{cc::state::ActionType::RemoveTask, std::string{"task-1"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::ClearActiveOverlays});
    state = store->get_state();
    EXPECT_FALSE(state.tasks.contains("task-1"));
    EXPECT_TRUE(state.active_overlays.empty());
}

TEST(StateStore, DispatchFeatureBucketParityActions) {
    auto store = make_test_store();

    cc::state::MCPState mcp;
    mcp.clients.push_back(cc::state::MCPServerConnection{
        .id = "mcp-1",
        .name = "MCP",
        .url = "stdio://mcp",
        .connected = true,
    });
    store->dispatch(cc::state::Action{cc::state::ActionType::UpdateMcpState, mcp});
    store->dispatch(cc::state::Action{cc::state::ActionType::IncrementMcpReconnectKey});

    cc::state::AppState::PluginsState plugins;
    plugins.enabled.push_back(cc::state::LoadedPlugin{
        .id = "plugin-1",
        .name = "Plugin",
        .version = "1.0.0",
        .enabled = true,
        .commands = {},
        .tools = {},
    });
    store->dispatch(cc::state::Action{cc::state::ActionType::UpdatePluginsState, plugins});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetPluginsNeedRefresh, true});

    cc::state::SpeculationState speculation;
    speculation.status = cc::state::SpeculationStatus::Active;
    speculation.id = "spec-1";
    store->dispatch(cc::state::Action{cc::state::ActionType::SetSpeculationState, speculation});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetSpeculationTimeSaved, 1234LL});

    cc::state::AppState::SkillImprovementState::Suggestion suggestion;
    suggestion.skill_name = "state";
    store->dispatch(cc::state::Action{cc::state::ActionType::SetSkillSuggestion, std::optional{suggestion}});
    store->dispatch(cc::state::Action{cc::state::ActionType::IncrementAuthVersion});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetEffortValue, std::optional<std::string>{"high"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetAdvisorModel, std::optional<std::string>{"advisor"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetUltraplanLaunching, true});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetUltraplanSessionUrl,
        std::optional<std::string>{"http://ultraplan/session"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetUltraplanMode, true});

    auto state = store->get_state();
    ASSERT_EQ(state.mcp.clients.size(), 1u);
    EXPECT_EQ(state.mcp.clients.front().id, "mcp-1");
    EXPECT_EQ(state.mcp.plugin_reconnect_key, 1u);
    ASSERT_EQ(state.plugins.enabled.size(), 1u);
    EXPECT_EQ(state.plugins.enabled.front().id, "plugin-1");
    EXPECT_TRUE(state.plugins.needs_refresh);
    EXPECT_EQ(state.speculation.status, cc::state::SpeculationStatus::Active);
    EXPECT_EQ(state.speculation.id, "spec-1");
    EXPECT_EQ(state.speculation_session_time_saved_ms, 1234LL);
    ASSERT_TRUE(state.skill_improvement.suggestion.has_value());
    EXPECT_EQ(state.skill_improvement.suggestion->skill_name, "state");
    EXPECT_EQ(state.auth_version, 1u);
    EXPECT_EQ(state.effort_value, std::optional<std::string>{"high"});
    EXPECT_EQ(state.advisor_model, std::optional<std::string>{"advisor"});
    EXPECT_TRUE(state.ultraplan_launching);
    EXPECT_EQ(state.ultraplan_session_url, std::optional<std::string>{"http://ultraplan/session"});
    EXPECT_TRUE(state.is_ultraplan_mode);
}

TEST(StateStore, DispatchPendingRequestPromptAndInboxParityActions) {
    auto store = make_test_store();

    cc::state::AppState::InitialMessage initial{
        .message = std::get<cc::core::UserMessage>(make_user_message("initial", "start")),
        .clear_context = true,
        .mode = cc::state::PermissionMode::Plan,
        .allowed_prompts = {"plan"},
    };
    store->dispatch(cc::state::Action{cc::state::ActionType::SetInitialMessage, initial});

    cc::state::AppState::WorkerSandboxPermissions::PermissionRequest permission{
        .request_id = "perm-1",
        .worker_id = "worker-1",
        .worker_name = "Worker",
        .worker_color = "blue",
        .host = "localhost",
        .created_at = std::chrono::system_clock::now(),
    };
    store->dispatch(cc::state::Action{cc::state::ActionType::AddSandboxPermissionRequest, permission});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetSelectedSandboxPermissionIndex,
        std::size_t{4}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetPendingWorkerRequest,
        std::optional{cc::state::AppState::PendingWorkerRequest{
            .tool_name = "Bash",
            .tool_use_id = "tool-1",
            .description = "run command",
        }}});
    store->dispatch(cc::state::Action{
        cc::state::ActionType::SetPendingSandboxRequest,
        std::optional{cc::state::AppState::PendingSandboxRequest{
            .request_id = "sandbox-1",
            .host = "localhost",
        }}});

    cc::state::AppState::PromptSuggestionState prompt;
    prompt.text = "try this";
    prompt.prompt_id = "prompt-1";
    prompt.shown_at = std::chrono::system_clock::now();
    store->dispatch(cc::state::Action{cc::state::ActionType::SetPromptSuggestion, prompt});

    cc::state::AppState::InboxState::InboxMessage inbox_message{
        .id = "inbox-1",
        .from = "agent",
        .text = "done",
        .timestamp = "now",
        .status = "unread",
        .color = "green",
        .summary = "summary",
    };
    store->dispatch(cc::state::Action{cc::state::ActionType::AddInboxMessage, inbox_message});

    auto state = store->get_state();
    ASSERT_TRUE(state.initial_message.has_value());
    EXPECT_TRUE(state.initial_message->clear_context);
    ASSERT_EQ(state.worker_sandbox_permissions.queue.size(), 1u);
    EXPECT_EQ(state.worker_sandbox_permissions.queue.front().request_id, "perm-1");
    EXPECT_EQ(state.worker_sandbox_permissions.selected_index, 4u);
    ASSERT_TRUE(state.pending_worker_request.has_value());
    EXPECT_EQ(state.pending_worker_request->tool_name, "Bash");
    ASSERT_TRUE(state.pending_sandbox_request.has_value());
    EXPECT_EQ(state.pending_sandbox_request->request_id, "sandbox-1");
    EXPECT_EQ(state.prompt_suggestion.text, std::optional<std::string>{"try this"});
    ASSERT_EQ(state.inbox.messages.size(), 1u);
    EXPECT_EQ(state.inbox.messages.front().id, "inbox-1");

    store->dispatch(cc::state::Action{cc::state::ActionType::ClearInitialMessage});
    store->dispatch(cc::state::Action{cc::state::ActionType::RemoveSandboxPermissionRequest, std::string{"perm-1"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::ClearPromptSuggestion});
    store->dispatch(cc::state::Action{cc::state::ActionType::RemoveInboxMessage, std::string{"inbox-1"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::AddInboxMessage, inbox_message});
    store->dispatch(cc::state::Action{cc::state::ActionType::ClearInboxMessages});

    state = store->get_state();
    EXPECT_FALSE(state.initial_message.has_value());
    EXPECT_TRUE(state.worker_sandbox_permissions.queue.empty());
    EXPECT_FALSE(state.prompt_suggestion.text.has_value());
    EXPECT_FALSE(state.prompt_suggestion.prompt_id.has_value());
    EXPECT_TRUE(state.inbox.messages.empty());
}

TEST(StateStore, UnsupportedSideEffectActionsRemainNoOps) {
    auto store = make_test_store();
    auto before = store->get_state();

    store->dispatch(cc::state::Action{cc::state::ActionType::EnableTool, std::string{"Bash"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::DisableTool, std::string{"Bash"}});
    store->dispatch(cc::state::Action{cc::state::ActionType::SaveState});
    store->dispatch(cc::state::Action{cc::state::ActionType::LoadState});
    store->dispatch(cc::state::Action{cc::state::ActionType::ClearSavedState});

    auto after = store->get_state();
    EXPECT_EQ(before.tool_permission_context.allowed_tools.size(), after.tool_permission_context.allowed_tools.size());
    for (const auto& tool : before.tool_permission_context.allowed_tools) {
        EXPECT_TRUE(after.tool_permission_context.allowed_tools.contains(tool));
    }
    EXPECT_EQ(before.tool_permission_context.denied_tools.size(), after.tool_permission_context.denied_tools.size());
    for (const auto& tool : before.tool_permission_context.denied_tools) {
        EXPECT_TRUE(after.tool_permission_context.denied_tools.contains(tool));
    }
    EXPECT_EQ(before.settings.model, after.settings.model);
    EXPECT_EQ(before.messages.size(), after.messages.size());
}

TEST(StateStore, MiddlewareSupport) {
    auto store = make_test_store();
    int middleware_count = 0;
    
    store->add_middleware([&middleware_count](cc::state::DispatchFn next) {
        return [next = std::move(next), &middleware_count](const cc::state::Action& action) {
            middleware_count++;
            next(action);
        };
    });
    
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    EXPECT_EQ(middleware_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(Selectors, IsVerbose) {
    auto state = cc::state::get_default_app_state();
    state.verbose = true;
    
    EXPECT_TRUE(cc::state::selectors::is_verbose(state));
}

TEST(Selectors, IsLoading) {
    auto state = cc::state::get_default_app_state();
    state.is_loading = true;
    
    EXPECT_TRUE(cc::state::selectors::is_loading(state));
}

TEST(Selectors, MemoizedSelector) {
    auto state = cc::state::get_default_app_state();
    int compute_count = 0;
    
    cc::state::selectors::MemoizedSelector<bool, bool> selector(
        [](const auto& s) {
            return s.verbose;
        },
        [&compute_count](const auto& s) {
            compute_count++;
            return s.verbose;
        }
    );
    

    auto result1 = selector.select(state);
    auto result2 = selector.select(state);
    EXPECT_EQ(result1, result2);
    EXPECT_EQ(compute_count, 1);
    

    state.verbose = true;
    auto result3 = selector.select(state);
    EXPECT_TRUE(result3);
    EXPECT_EQ(compute_count, 2);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(OnChangeAppState, StateChangeRegistry) {
    cc::state::on_change::StateChangeRegistry registry;
    int callback_count = 0;
    
    registry.register_callback([&callback_count](const auto&, const auto&) {
        callback_count++;
    });
    
    auto state1 = cc::state::get_default_app_state();
    auto state2 = cc::state::get_default_app_state();
    state2.verbose = true;
    
    registry.run_callbacks(state1, state2);
    EXPECT_EQ(callback_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(FTXUIIntegration, VerboseIndicator) {
    cc::state::ftxui::VerboseIndicator indicator;
    
    auto store = make_shared_test_store();
    indicator.connect(store);
    
    EXPECT_EQ(indicator.get_text(), "");
    
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    // Note: In real usage, the component would need to process the state change
    // This test verifies basic construction and API
    EXPECT_TRUE(indicator.get_last_state().verbose);
}

TEST(FTXUIIntegration, LoadingIndicator) {
    cc::state::ftxui::LoadingIndicator indicator;
    
    auto store = make_shared_test_store();
    indicator.connect(store);
    
    EXPECT_EQ(indicator.get_text(), "");
}

TEST(FTXUIIntegration, MessageCounter) {
    cc::state::ftxui::MessageCounter counter;
    
    auto store = make_shared_test_store();
    counter.connect(store);
    

    EXPECT_EQ(counter.get_last_state().messages.size(), 0);
}

TEST(FTXUIIntegration, ReactiveScreenManager) {
    auto store = make_shared_test_store();
    auto manager = cc::state::ftxui::make_reactive_screen_manager(store);
    
    auto indicator = std::make_shared<cc::state::ftxui::VerboseIndicator>();
    manager->add_component(indicator);
    
    EXPECT_EQ(manager->get_store(), store);
    manager->clear_components();
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(Persistence, StatePersistenceAPI) {

    auto state_file = std::filesystem::temp_directory_path() / "cc_repl_test_state.json";
    cc::state::persistence::StatePersistence persistence(state_file);
    
    auto state = cc::state::get_default_app_state();
    state.verbose = true;
    state.is_loading = false;
    

    auto save_result = persistence.save_state(state);
    ASSERT_TRUE(save_result.has_value());

    auto loaded = persistence.load_state();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->verbose);
    EXPECT_FALSE(loaded->is_loading);

    (void)persistence.delete_state();
}

TEST(Persistence, RoundTripsAllPersistedFields) {
    auto src = cc::state::get_default_app_state();
    // Flip every persisted field away from its default.
    src.verbose = true;
    src.compact_mode = true;
    src.show_thinking = true;
    src.fast_mode = true;
    src.thinking_enabled = false;             // default is true
    src.prompt_suggestion_enabled = false;     // default is true
    src.kairos_enabled = true;
    src.is_ultraplan_mode = true;
    src.ultraplan_launching = true;
    src.is_brief_only = true;
    src.show_teammate_message_preview = true;
    src.working_directory = "/tmp/cc-roundtrip";
    src.view_selection_mode = "viewing-agent";
    src.selected_ip_agent_index = 7;
    src.coordinator_task_index = 3;
    src.auth_version = 42;
    src.remote_background_task_count = 9;
    src.main_loop_model = "claude-opus-4-8";
    src.advisor_model = "claude-haiku-4-5";
    src.effort_value = "high";
    src.status_line_text = "custom status";

    auto serialized = cc::state::persistence::serialize_state(src);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().format();

    auto parsed = cc::state::persistence::deserialize_state(*serialized);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().format();
    const auto& dst = *parsed;

    EXPECT_EQ(dst.verbose, src.verbose);
    EXPECT_EQ(dst.compact_mode, src.compact_mode);
    EXPECT_EQ(dst.show_thinking, src.show_thinking);
    EXPECT_EQ(dst.fast_mode, src.fast_mode);
    EXPECT_EQ(dst.thinking_enabled, src.thinking_enabled);
    EXPECT_EQ(dst.prompt_suggestion_enabled, src.prompt_suggestion_enabled);
    EXPECT_EQ(dst.kairos_enabled, src.kairos_enabled);
    EXPECT_EQ(dst.is_ultraplan_mode, src.is_ultraplan_mode);
    EXPECT_EQ(dst.ultraplan_launching, src.ultraplan_launching);
    EXPECT_EQ(dst.is_brief_only, src.is_brief_only);
    EXPECT_EQ(dst.show_teammate_message_preview, src.show_teammate_message_preview);
    EXPECT_EQ(dst.working_directory, src.working_directory);
    EXPECT_EQ(dst.view_selection_mode, src.view_selection_mode);
    EXPECT_EQ(dst.selected_ip_agent_index, src.selected_ip_agent_index);
    EXPECT_EQ(dst.coordinator_task_index, src.coordinator_task_index);
    EXPECT_EQ(dst.auth_version, src.auth_version);
    EXPECT_EQ(dst.remote_background_task_count, src.remote_background_task_count);
    ASSERT_TRUE(dst.main_loop_model.has_value());  EXPECT_EQ(*dst.main_loop_model, *src.main_loop_model);
    ASSERT_TRUE(dst.advisor_model.has_value());    EXPECT_EQ(*dst.advisor_model, *src.advisor_model);
    ASSERT_TRUE(dst.effort_value.has_value());     EXPECT_EQ(*dst.effort_value, *src.effort_value);
    ASSERT_TRUE(dst.status_line_text.has_value()); EXPECT_EQ(*dst.status_line_text, *src.status_line_text);
}

TEST(Persistence, AbsentOptionalStringsStayDefault) {
    auto src = cc::state::get_default_app_state();
    auto serialized = cc::state::persistence::serialize_state(src);
    ASSERT_TRUE(serialized.has_value());
    auto parsed = cc::state::persistence::deserialize_state(*serialized);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->main_loop_model.has_value());
    EXPECT_FALSE(parsed->advisor_model.has_value());
    EXPECT_FALSE(parsed->effort_value.has_value());
    EXPECT_FALSE(parsed->status_line_text.has_value());
}

TEST(Persistence, LoadsLegacyV1ShapeWithMissingFields) {
    // Minimal legacy v1 object. thinking_enabled and auth_version were
    // previously written-but-dropped; this proves they now round-trip, while
    // fields absent from the legacy blob keep their defaults.
    std::string legacy = R"({"verbose":true,"thinking_enabled":false,"auth_version":5,"schema_version":1})";
    auto parsed = cc::state::persistence::deserialize_state(legacy);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().format();
    EXPECT_TRUE(parsed->verbose);
    EXPECT_FALSE(parsed->thinking_enabled);
    EXPECT_EQ(parsed->auth_version, 5u);
    EXPECT_EQ(parsed->view_selection_mode, "none");
    EXPECT_FALSE(parsed->main_loop_model.has_value());
}

TEST(Persistence, WritesCurrentSchemaVersion) {
    auto s = cc::state::get_default_app_state();
    auto serialized = cc::state::persistence::serialize_state(s);
    ASSERT_TRUE(serialized.has_value());
    EXPECT_NE(serialized->find("\"schema_version\":2"), std::string::npos);
    EXPECT_EQ(cc::state::persistence::kCurrentStateSchemaVersion, 2);
}

TEST(Persistence, MigratesV1EmptyViewModeToNone) {
    std::string v1 = R"({"schema_version":1,"view_selection_mode":""})";
    auto parsed = cc::state::persistence::deserialize_state(v1);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->view_selection_mode, "none"); // v1->v2 migration normalised the empty sentinel
    // An explicit v1 value is preserved through migration.
    std::string v1_named = R"({"schema_version":1,"view_selection_mode":"viewing-agent"})";
    auto named = cc::state::persistence::deserialize_state(v1_named);
    ASSERT_TRUE(named.has_value());
    EXPECT_EQ(named->view_selection_mode, "viewing-agent");
}

TEST(Persistence, ValidateStateAcceptsDefaultsRejectsBadValues) {
    auto good = cc::state::get_default_app_state();
    EXPECT_TRUE(cc::state::persistence::validate_state(good).has_value());

    auto bad_index = cc::state::get_default_app_state();
    bad_index.selected_ip_agent_index = -5;
    EXPECT_FALSE(cc::state::persistence::validate_state(bad_index).has_value());

    auto bad_cost = cc::state::get_default_app_state();
    bad_cost.total_cost_usd = -1.0;
    EXPECT_FALSE(cc::state::persistence::validate_state(bad_cost).has_value());
}

TEST(Persistence, DeserializeRejectsInvalidIndices) {
    std::string malformed = R"({"selected_ip_agent_index":-5,"schema_version":2})";
    auto parsed = cc::state::persistence::deserialize_state(malformed);
    ASSERT_FALSE(parsed.has_value());
}

TEST(StoreUndoRedo, RoundTripsDispatchedActions) {
    auto store = make_test_store();
    store->enable_undo();
    EXPECT_FALSE(store->can_undo());

    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});   // false -> true
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, false});  // true -> false
    EXPECT_FALSE(store->get_state().verbose);

    ASSERT_TRUE(store->can_undo());
    store->undo();
    EXPECT_TRUE(store->get_state().verbose);
    store->undo();
    EXPECT_FALSE(store->get_state().verbose);
    EXPECT_FALSE(store->can_undo());

    ASSERT_TRUE(store->can_redo());
    store->redo();
    EXPECT_TRUE(store->get_state().verbose);
    store->redo();
    EXPECT_FALSE(store->get_state().verbose);
    EXPECT_FALSE(store->can_redo());
}

TEST(StoreUndoRedo, CapacityBoundsHistoryToOneLevel) {
    auto store = make_test_store();
    store->enable_undo(1);
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, false});
    store->undo();
    EXPECT_TRUE(store->get_state().verbose); // only the most recent snapshot survives
    EXPECT_FALSE(store->can_undo());
}

TEST(StoreUndoRedo, NewActionClearsRedoStack) {
    auto store = make_test_store();
    store->enable_undo();
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    store->undo(); // verbose=false, redo has the true snapshot
    ASSERT_TRUE(store->can_redo());
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    EXPECT_FALSE(store->can_redo()); // a new dispatch clears redo
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionHistory, SaveAllPersistsCreatedConversationIds) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_save_test.json";
    std::filesystem::remove(storage_path);

    cc::core::ConversationStore store(storage_path.string());
    store.create_conversation();
    auto ids = store.get_conversation_ids();
    ASSERT_EQ(ids.size(), 1u);

    auto saved = store.save_all();
    ASSERT_TRUE(saved.has_value()) << saved.error().format();

    std::ifstream input(storage_path);
    ASSERT_TRUE(input.is_open());
    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find(ids.front()), std::string::npos);
    EXPECT_NE(json.find("active_conversation_id"), std::string::npos);

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresConversationIdsAndActiveSelection) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_load_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        store.create_conversation();
        auto ids = store.get_conversation_ids();
        ASSERT_EQ(ids.size(), 1u);
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    auto result = loaded.load_all();
    ASSERT_TRUE(result.has_value()) << result.error().format();

    auto loaded_ids = loaded.get_conversation_ids();
    ASSERT_EQ(loaded_ids.size(), 1u);
    EXPECT_TRUE(loaded.switch_conversation(loaded_ids.front()));

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresSavedMessages) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_messages_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        auto* conversation = store.create_conversation();
        conversation->add_message(cc::core::UserMessage{
            cc::core::MessageBase{
                cc::core::MessageId{"msg_user_1"},
                std::chrono::system_clock::now(),
                {cc::core::TextBlock{"hello from persisted history"}}
            }
        });
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto* active = loaded.get_active_conversation();
    auto messages = active->get_messages();

    ASSERT_EQ(messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::UserMessage>(messages.front()));
    const auto& user = std::get<cc::core::UserMessage>(messages.front());
    ASSERT_EQ(user.content.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::TextBlock>(user.content.front()));
    EXPECT_EQ(std::get<cc::core::TextBlock>(user.content.front()).text,
              "hello from persisted history");

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresCompactBoundaryMetadata) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_compact_boundary_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        auto* conversation = store.create_conversation();
        conversation->add_message(cc::core::SystemMessage{
            cc::core::MessageBase{
                cc::core::MessageId{"compact-boundary-1"},
                std::chrono::system_clock::now(),
                {cc::core::TextBlock{"Conversation compacted."}}
            },
            std::nullopt,
            std::string{"compact_boundary"},
            cc::core::CompactMetadata{
                .trigger = "manual",
                .pre_tokens = 1234,
                .preserved_segment = cc::core::CompactPreservedSegment{
                    .head_uuid = "head-message",
                    .anchor_uuid = "summary-message",
                    .tail_uuid = "tail-message",
                },
            },
        });
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto* active = loaded.get_active_conversation();
    auto messages = active->get_messages();

    ASSERT_EQ(messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::SystemMessage>(messages.front()));
    const auto& boundary = std::get<cc::core::SystemMessage>(messages.front());
    ASSERT_TRUE(boundary.subtype.has_value());
    EXPECT_EQ(*boundary.subtype, "compact_boundary");
    ASSERT_TRUE(boundary.compact_metadata.has_value());
    EXPECT_EQ(boundary.compact_metadata->trigger, "manual");
    EXPECT_EQ(boundary.compact_metadata->pre_tokens, 1234u);
    ASSERT_TRUE(boundary.compact_metadata->preserved_segment.has_value());
    EXPECT_EQ(boundary.compact_metadata->preserved_segment->head_uuid, "head-message");
    EXPECT_EQ(boundary.compact_metadata->preserved_segment->anchor_uuid, "summary-message");
    EXPECT_EQ(boundary.compact_metadata->preserved_segment->tail_uuid, "tail-message");

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresSnipMetadata) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_snip_metadata_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        auto* conversation = store.create_conversation();
        conversation->add_message(cc::core::SystemMessage{
            cc::core::MessageBase{
                cc::core::MessageId{"snip-boundary-1"},
                std::chrono::system_clock::now(),
                {cc::core::TextBlock{"Conversation snipped."}}
            },
            std::nullopt,
            std::string{"snip_boundary"},
            std::nullopt,
            cc::core::SnipMetadata{
                .removed_uuids = {"old-user-1", "old-assistant-1"},
            },
        });
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto* active = loaded.get_active_conversation();
    auto messages = active->get_messages();

    ASSERT_EQ(messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::SystemMessage>(messages.front()));
    const auto& boundary = std::get<cc::core::SystemMessage>(messages.front());
    ASSERT_TRUE(boundary.subtype.has_value());
    EXPECT_EQ(*boundary.subtype, "snip_boundary");
    ASSERT_TRUE(boundary.snip_metadata.has_value());
    ASSERT_EQ(boundary.snip_metadata->removed_uuids.size(), 2u);
    EXPECT_EQ(boundary.snip_metadata->removed_uuids[0], "old-user-1");
    EXPECT_EQ(boundary.snip_metadata->removed_uuids[1], "old-assistant-1");

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresImageAndDocumentBlocks) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_rich_content_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        auto* conversation = store.create_conversation();
        conversation->add_message(cc::core::UserMessage{
            cc::core::MessageBase{
                cc::core::MessageId{"msg_user_rich"},
                std::chrono::system_clock::now(),
                {
                    cc::core::TextBlock{"rich content"},
                    cc::core::ImageBlock{"image/png", "iVBORw0KGgo="},
                    cc::core::DocumentBlock{"application/pdf", "JVBERi0xLjQ="},
                }
            }
        });
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto* active = loaded.get_active_conversation();
    auto messages = active->get_messages();

    ASSERT_EQ(messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::UserMessage>(messages.front()));
    const auto& user = std::get<cc::core::UserMessage>(messages.front());
    ASSERT_EQ(user.content.size(), 3u);

    ASSERT_TRUE(std::holds_alternative<cc::core::ImageBlock>(user.content[1]));
    const auto& image = std::get<cc::core::ImageBlock>(user.content[1]);
    EXPECT_EQ(image.media_type, "image/png");
    EXPECT_EQ(image.data, "iVBORw0KGgo=");

    ASSERT_TRUE(std::holds_alternative<cc::core::DocumentBlock>(user.content[2]));
    const auto& document = std::get<cc::core::DocumentBlock>(user.content[2]);
    EXPECT_EQ(document.media_type, "application/pdf");
    EXPECT_EQ(document.data, "JVBERi0xLjQ=");

    std::filesystem::remove(storage_path);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliUpdate, DownloadUpdateCopiesFileUrlPayload) {
    auto source_path = std::filesystem::temp_directory_path() /
        "cc_repl_update_source.bin";
    std::filesystem::remove(source_path);
    {
        std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
        output << "real update payload";
    }

    auto downloaded = cc::cli::download_update("file://" + source_path.string());
    ASSERT_TRUE(downloaded.has_value()) << downloaded.error();
    ASSERT_TRUE(std::filesystem::exists(*downloaded));

    std::ifstream input(*downloaded, std::ios::binary);
    std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(payload, "real update payload");

    std::filesystem::remove(source_path);
    std::filesystem::remove(*downloaded);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(McpAuth, FetchConfiguredMetadataFromFileUrl) {
    auto metadata_path = std::filesystem::temp_directory_path() /
        "cc_repl_mcp_oauth_metadata.json";
    std::filesystem::remove(metadata_path);
    {
        std::ofstream output(metadata_path, std::ios::trunc);
        output << R"({
            "authorization_endpoint":"https://auth.example.com/authorize",
            "token_endpoint":"https://auth.example.com/token",
            "scope":"openid profile"
        })";
    }

    auto metadata = cc::services::mcp::fetch_auth_server_metadata(
        "test-server",
        "https://mcp.example.com/sse",
        "file://" + metadata_path.string());

    ASSERT_TRUE(metadata.has_value()) << metadata.error().format();
    ASSERT_TRUE(metadata->has_value());
    EXPECT_EQ((*metadata)->authorization_endpoint, "https://auth.example.com/authorize");
    EXPECT_EQ((*metadata)->token_endpoint, "https://auth.example.com/token");
    ASSERT_TRUE((*metadata)->scope.has_value());
    EXPECT_EQ(*(*metadata)->scope, "openid profile");

    std::filesystem::remove(metadata_path);
}

TEST(McpAuth, XaaFlowDoesNotReturnUnimplementedError) {
    unsetenv("CLAUDE_CODE_ENABLE_XAA");

    cc::services::mcp::McpServerConfig server_config{
        .type = "http",
        .url = "https://mcp.example.com/mcp",
        .headers = {},
        .oauth = cc::services::mcp::McpOAuthConfig{
            .auth_server_metadata_url = std::nullopt,
            .callback_port = std::nullopt,
            .client_id = std::nullopt,
            .xaa = true}
    };

    auto result = cc::services::mcp::perform_mcp_oauth_flow(
        "test-server",
        server_config,
        [](const std::string&) {});

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().code(), cc::utils::ErrorCode::unimplemented);
    EXPECT_NE(result.error().message().find("XAA is not enabled"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemPrompts, ComputeSimpleEnvInfoIncludesDynamicRuntimeDetails) {
    auto env_info = cc::constants::prompts::compute_simple_env_info(
        "claude-sonnet-4-6",
        {"/tmp/cc-repl-extra"});

    EXPECT_NE(env_info.find("# Environment"), std::string::npos);
    EXPECT_NE(env_info.find("Primary working directory:"), std::string::npos);
    EXPECT_NE(env_info.find("Additional working directories:"), std::string::npos);
    EXPECT_NE(env_info.find("/tmp/cc-repl-extra"), std::string::npos);
    EXPECT_NE(env_info.find("Assistant knowledge cutoff is August 2025."), std::string::npos);
}

TEST(SystemPrompts, GetSystemPromptAssemblesStaticAndDynamicSections) {
    cc::constants::prompts::SystemPromptOptions options{
        .model = "claude-opus-4-6",
        .enabled_tools = {"Read", "Write"},
        .additional_working_directories = {},
        .simple = false,
        .use_global_cache_boundary = true,
    };

    auto sections = cc::constants::prompts::get_system_prompt(options);
    ASSERT_GE(sections.size(), 6u);

    auto joined = std::accumulate(std::next(sections.begin()), sections.end(), sections.front(),
        [](std::string acc, const std::string& section) {
            acc += "\n";
            acc += section;
            return acc;
        });

    EXPECT_NE(joined.find("You are Claude Code"), std::string::npos);
    EXPECT_NE(joined.find("# Tone and style"), std::string::npos);
    EXPECT_NE(joined.find(cc::constants::prompts::system_prompt_dynamic_boundary), std::string::npos);
    EXPECT_NE(joined.find("# Environment"), std::string::npos);
    EXPECT_NE(joined.find("Read"), std::string::npos);
    EXPECT_NE(joined.find("Write"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Store genericity — the Store template must be instantiable with a State
// other than AppState (dispatch/undo/redo/subscribers work generically; the
// AppState-specific persistence/change-registry hooks are compile-time-gated).
// ---------------------------------------------------------------------------

namespace {

struct CounterState {
    int value = 0;
};

[[nodiscard]] inline CounterState counter_reducer(const CounterState& s, const cc::state::Action& a) {
    CounterState next = s;
    if (a.type == cc::state::ActionType::SetLoading) {
        if (auto v = a.get_payload<bool>(); v && *v) next.value += 1;
    }
    return next;
}

} // namespace

TEST(StoreGenericity, IsGenericOverStateAndSupportsUndoRedo) {
    using CounterStore = cc::state::Store<CounterState, decltype(&counter_reducer)>;
    CounterStore store{CounterState{0}, &counter_reducer};
    EXPECT_EQ(store.get_state().value, 0);

    store.enable_undo();
    store.dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});  // value -> 1
    store.dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});  // value -> 2
    EXPECT_EQ(store.get_state().value, 2);

    // Generic undo/redo works on a non-AppState State.
    ASSERT_TRUE(store.can_undo());
    store.undo();
    EXPECT_EQ(store.get_state().value, 1);
    store.undo();
    EXPECT_EQ(store.get_state().value, 0);
    EXPECT_FALSE(store.can_undo());

    store.redo();
    EXPECT_EQ(store.get_state().value, 1);

    // Generic subscriber fan-out works.
    int notifications = 0;
    auto sub = store.subscribe([&](const CounterState&, const CounterState&) { ++notifications; });
    store.dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});
    EXPECT_EQ(notifications, 1);
    store.unsubscribe(sub);
}
