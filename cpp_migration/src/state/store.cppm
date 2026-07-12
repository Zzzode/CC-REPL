/// @file store.cppm
/// @brief Redux-like state management store for the Claude Code REPL.
/// Provides a generic Store class template with actions, reducers,
/// middleware pipeline, subscriptions, and async thunk support.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <optional>
#include <expected>
#include <format>
#include <concepts>
#include <variant>
#include <type_traits>
#include <any>
#include <queue>
#include <future>
#include <chrono>

export module cc.state.store;

import cc.types.types;
import cc.state.app_state;
import cc.state.selectors;
import cc.state.persistence;
import cc.state.on_change;

export namespace cc::state {

// ============================================================
// Action System
// ============================================================

/// Extended action type identifiers
enum class ActionType : std::uint16_t {
    // Message actions
    AddMessage,
    ClearMessages,
    UpdateLastMessage,

    // Loading/streaming
    SetLoading,
    SetStreaming,

    // Error handling
    SetError,
    ClearError,

    // Token usage
    UpdateUsage,

    // Model management
    SwitchModel,
    SetMainLoopModel,

    // Permission actions
    GrantPermission,
    RevokePermission,
    SetPermissionMode,

    // Tool management
    EnableTool,
    DisableTool,

    // UI state
    ToggleCompactMode,
    ToggleThinking,
    SetSlashCommand,
    AddNotification,
    DismissNotification,
    SetStatusLineText,
    SetExpandedView,
    SetFooterSelection,
    SetSpinnerTip,
    SetBriefOnly,
    SetShowTeammatePreview,
    SetSelectedAgentIndex,
    SetCoordinatorTaskIndex,
    SetViewSelectionMode,

    // Session lifecycle
    ResetSession,
    UpdateActivity,
    SetWorkingDirectory,
    AddAllowedDirectory,

    // Settings
    UpdateSettings,
    SetSettingsModel,
    SetSettingsTheme,
    SetSettingsVerbose,
    SetOutputStyle,
    SetDisableAllHooks,
    SetStatusLineEnabled,
    SetStatusLineCommand,
    SetStatusLinePadding,
    SetVerbose,
    SetFastMode,
    SetThinkingEnabled,
    SetPromptSuggestionEnabled,

    // Bridge
    SetBridgeEnabled,
    SetBridgeExplicit,
    SetBridgeOutboundOnly,
    SetBridgeConnected,
    SetBridgeSessionActive,
    SetBridgeReconnecting,
    SetBridgeConnectUrl,
    SetBridgeSessionUrl,
    SetBridgeEnvironmentId,
    SetBridgeSessionId,
    SetBridgeError,
    SetBridgeInitialName,
    SetShowRemoteCallout,

    // Remote
    SetRemoteSessionUrl,
    SetRemoteConnectionStatus,
    SetRemoteBackgroundTaskCount,

    // Tasks
    AddTask,
    RemoveTask,
    UpdateTask,
    SetForegroundedTaskId,
    SetViewingAgentTaskId,

    // Agent
    RegisterAgentName,
    SetAgent,
    SetKairosEnabled,

    // Companion
    SetCompanionReaction,
    SetCompanionPetTime,

    // MCP
    UpdateMcpState,
    IncrementMcpReconnectKey,

    // Plugins
    UpdatePluginsState,
    SetPluginsNeedRefresh,

    // Speculation
    SetSpeculationState,
    SetSpeculationTimeSaved,

    // Skill improvement
    SetSkillSuggestion,

    // Auth
    IncrementAuthVersion,

    // Initial message
    SetInitialMessage,
    ClearInitialMessage,

    // Effort
    SetEffortValue,

    // Overlays
    AddActiveOverlay,
    RemoveActiveOverlay,
    ClearActiveOverlays,

    // Advisor
    SetAdvisorModel,

    // Ultraplan
    SetUltraplanLaunching,
    SetUltraplanSessionUrl,
    SetUltraplanPendingChoice,
    SetUltraplanLaunchPending,
    SetUltraplanMode,

    // Worker sandbox
    AddSandboxPermissionRequest,
    RemoveSandboxPermissionRequest,
    SetSelectedSandboxPermissionIndex,
    SetPendingWorkerRequest,
    SetPendingSandboxRequest,

    // Prompt suggestion
    SetPromptSuggestion,
    ClearPromptSuggestion,

    // Inbox
    AddInboxMessage,
    RemoveInboxMessage,
    ClearInboxMessages,

    // Persistence
    SaveState,
    LoadState,
    ClearSavedState,

    // Batch actions
    Batch,
};

/// Extended action carrying a type tag and optional payload
struct Action {
    ActionType type;
    std::any payload;

    /// Construct action with no payload
    explicit Action(ActionType t) : type(t) {}

    /// Construct action with typed payload
    template <typename T>
    Action(ActionType t, T&& data) : type(t), payload(std::forward<T>(data)) {}

    /// Extract payload as a specific type (returns nullopt if wrong type)
    template <typename T>
    [[nodiscard]] std::optional<T> get_payload() const {
        if (auto* p = std::any_cast<T>(&payload)) {
            return *p;
        }
        return std::nullopt;
    }

    /// Check if payload holds a specific type
    template <typename T>
    [[nodiscard]] bool has_payload() const {
        return payload.type() == typeid(T);
    }
};

// ============================================================
// Reducer
// ============================================================

/// Reducer concept: takes state + action, produces new state
template <typename F, typename State>
concept Reducer = std::invocable<F, const State&, const Action&> &&
                  std::same_as<std::invoke_result_t<F, const State&, const Action&>, State>;

/// Extended reducer for AppState
[[nodiscard]] inline AppState app_reducer(const AppState& state, const Action& action) {
    AppState next = state;

    switch (action.type) {
        // ========================================
        // Message actions
        // ========================================
        case ActionType::AddMessage: {
            if (auto msg = action.get_payload<cc::core::Message>()) {
                return with_message(state, std::move(*msg));
            }
            break;
        }
        case ActionType::ClearMessages: {
            next.messages.clear();
            break;
        }
        case ActionType::UpdateLastMessage: {
            if (auto msg = action.get_payload<cc::core::Message>(); msg && !next.messages.empty()) {
                next.messages.back() = std::move(*msg);
                next.last_activity = std::chrono::system_clock::now();
            }
            break;
        }

        // ========================================
        // Loading/streaming
        // ========================================
        case ActionType::SetLoading: {
            if (auto val = action.get_payload<bool>()) {
                return with_loading(state, *val);
            }
            break;
        }
        case ActionType::SetStreaming: {
            if (auto val = action.get_payload<bool>()) {
                next.is_streaming = *val;
            }
            break;
        }

        // ========================================
        // Error handling
        // ========================================
        case ActionType::SetError: {
            if (auto msg = action.get_payload<std::string>()) {
                return with_error(state, std::move(*msg));
            }
            break;
        }
        case ActionType::ClearError: {
            return clear_error(state);
        }

        // ========================================
        // Token usage
        // ========================================
        case ActionType::UpdateUsage: {
            if (auto usage = action.get_payload<cc::core::TokenUsage>()) {
                return with_usage(state, *usage);
            }
            break;
        }

        // ========================================
        // Model management
        // ========================================
        case ActionType::SwitchModel: {
            if (auto model = action.get_payload<ModelConfig>()) {
                return with_model(state, std::move(*model));
            }
            break;
        }
        case ActionType::SetMainLoopModel: {
            if (auto model = action.get_payload<std::optional<std::string>>()) {
                next.main_loop_model = std::move(*model);
            }
            break;
        }

        // ========================================
        // Permission actions
        // ========================================
        case ActionType::GrantPermission: {
            if (auto tool = action.get_payload<std::string>()) {
                next.tool_permission_context.allowed_tools.insert(*tool);
                next.tool_permission_context.denied_tools.erase(*tool);
            }
            break;
        }
        case ActionType::RevokePermission: {
            if (auto tool = action.get_payload<std::string>()) {
                next.tool_permission_context.denied_tools.insert(*tool);
                next.tool_permission_context.allowed_tools.erase(*tool);
            }
            break;
        }
        case ActionType::SetPermissionMode: {
            if (auto mode = action.get_payload<PermissionMode>()) {
                return with_permission_mode(state, *mode);
            }
            break;
        }

        // ========================================
        // UI state
        // ========================================
        case ActionType::ToggleCompactMode: {
            next.compact_mode = !next.compact_mode;
            break;
        }
        case ActionType::ToggleThinking: {
            next.show_thinking = !next.show_thinking;
            break;
        }
        case ActionType::SetSlashCommand: {
            if (auto command = action.get_payload<std::optional<std::string>>()) {
                next.active_slash_command = std::move(*command);
            }
            break;
        }
        case ActionType::AddNotification: {
            if (auto notification = action.get_payload<std::string>()) {
                next.notifications.push_back(std::move(*notification));
            }
            break;
        }
        case ActionType::DismissNotification: {
            if (auto notification = action.get_payload<std::string>()) {
                std::erase(next.notifications, *notification);
            }
            break;
        }
        case ActionType::SetStatusLineText: {
            if (auto text = action.get_payload<std::optional<std::string>>()) {
                next.status_line_text = std::move(*text);
            }
            break;
        }
        case ActionType::SetVerbose: {
            if (auto val = action.get_payload<bool>()) {
                return with_verbose(state, *val);
            }
            break;
        }
        case ActionType::SetExpandedView: {
            if (auto view = action.get_payload<ExpandedView>()) {
                return with_expanded_view(state, *view);
            }
            break;
        }
        case ActionType::SetFastMode: {
            if (auto val = action.get_payload<bool>()) {
                next.fast_mode = *val;
            }
            break;
        }
        case ActionType::SetFooterSelection: {
            if (auto selection = action.get_payload<std::optional<FooterItem>>()) {
                next.footer_selection = *selection;
            }
            break;
        }
        case ActionType::SetSpinnerTip: {
            if (auto tip = action.get_payload<std::optional<std::string>>()) {
                next.spinner_tip = std::move(*tip);
            }
            break;
        }
        case ActionType::SetBriefOnly: {
            if (auto val = action.get_payload<bool>()) {
                next.is_brief_only = *val;
            }
            break;
        }
        case ActionType::SetShowTeammatePreview: {
            if (auto val = action.get_payload<bool>()) {
                next.show_teammate_message_preview = *val;
            }
            break;
        }
        case ActionType::SetSelectedAgentIndex: {
            if (auto index = action.get_payload<std::int32_t>()) {
                next.selected_ip_agent_index = *index;
            }
            break;
        }
        case ActionType::SetCoordinatorTaskIndex: {
            if (auto index = action.get_payload<std::int32_t>()) {
                next.coordinator_task_index = *index;
            }
            break;
        }
        case ActionType::SetViewSelectionMode: {
            if (auto mode = action.get_payload<std::string>()) {
                next.view_selection_mode = std::move(*mode);
            }
            break;
        }

        // ========================================
        // Session lifecycle
        // ========================================
        case ActionType::ResetSession: {
            next = get_default_app_state();
            // Keep some configuration from previous state
            next.current_model = state.current_model;
            next.available_models = state.available_models;
            next.tool_permission_context = state.tool_permission_context;
            next.working_directory = state.working_directory;
            next.allowed_directories = state.allowed_directories;
            next.settings = state.settings;
            break;
        }
        case ActionType::UpdateActivity: {
            next.last_activity = std::chrono::system_clock::now();
            break;
        }
        case ActionType::SetWorkingDirectory: {
            if (auto dir = action.get_payload<std::string>()) {
                next.working_directory = std::move(*dir);
            }
            break;
        }
        case ActionType::AddAllowedDirectory: {
            if (auto dir = action.get_payload<std::string>()) {
                // Avoid duplicates
                const auto& new_dir = *dir;
                bool found = false;
                for (const auto& d : next.allowed_directories) {
                    if (d == new_dir) { found = true; break; }
                }
                if (!found) {
                    next.allowed_directories.push_back(new_dir);
                }
            }
            break;
        }

        // ========================================
        // Settings
        // ========================================
        case ActionType::UpdateSettings: {
            if (auto settings = action.get_payload<Settings>()) {
                next.settings = std::move(*settings);
            }
            break;
        }
        case ActionType::SetSettingsModel: {
            if (auto model = action.get_payload<std::string>()) {
                next.settings.model = std::move(*model);
            }
            break;
        }
        case ActionType::SetSettingsTheme: {
            if (auto theme = action.get_payload<std::string>()) {
                next.settings.theme = std::move(*theme);
            }
            break;
        }
        case ActionType::SetSettingsVerbose: {
            if (auto val = action.get_payload<bool>()) {
                next.settings.verbose = *val;
            }
            break;
        }
        case ActionType::SetOutputStyle: {
            if (auto style = action.get_payload<std::string>()) {
                next.settings.output_style = std::move(*style);
            }
            break;
        }
        case ActionType::SetDisableAllHooks: {
            if (auto val = action.get_payload<bool>()) {
                next.settings.disable_all_hooks = *val;
            }
            break;
        }
        case ActionType::SetStatusLineEnabled: {
            if (auto val = action.get_payload<bool>()) {
                next.settings.status_line.enabled = *val;
            }
            break;
        }
        case ActionType::SetStatusLineCommand: {
            if (auto cmd = action.get_payload<std::string>()) {
                next.settings.status_line.command = std::move(*cmd);
            }
            break;
        }
        case ActionType::SetStatusLinePadding: {
            if (auto val = action.get_payload<int>()) {
                next.settings.status_line.padding = *val;
            }
            break;
        }
        case ActionType::SetThinkingEnabled: {
            if (auto val = action.get_payload<bool>()) {
                next.thinking_enabled = *val;
            }
            break;
        }
        case ActionType::SetPromptSuggestionEnabled: {
            if (auto val = action.get_payload<bool>()) {
                next.prompt_suggestion_enabled = *val;
            }
            break;
        }

        // ========================================
        // Bridge
        // ========================================
        case ActionType::SetBridgeEnabled: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_enabled = *val;
            break;
        }
        case ActionType::SetBridgeExplicit: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_explicit = *val;
            break;
        }
        case ActionType::SetBridgeOutboundOnly: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_outbound_only = *val;
            break;
        }
        case ActionType::SetBridgeConnected: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_connected = *val;
            break;
        }
        case ActionType::SetBridgeSessionActive: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_session_active = *val;
            break;
        }
        case ActionType::SetBridgeReconnecting: {
            if (auto val = action.get_payload<bool>()) next.repl_bridge_reconnecting = *val;
            break;
        }
        case ActionType::SetBridgeConnectUrl: {
            if (auto url = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_connect_url = std::move(*url);
            }
            break;
        }
        case ActionType::SetBridgeSessionUrl: {
            if (auto url = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_session_url = std::move(*url);
            }
            break;
        }
        case ActionType::SetBridgeEnvironmentId: {
            if (auto id = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_environment_id = std::move(*id);
            }
            break;
        }
        case ActionType::SetBridgeSessionId: {
            if (auto id = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_session_id = std::move(*id);
            }
            break;
        }
        case ActionType::SetBridgeError: {
            if (auto error = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_error = std::move(*error);
            }
            break;
        }
        case ActionType::SetBridgeInitialName: {
            if (auto name = action.get_payload<std::optional<std::string>>()) {
                next.repl_bridge_initial_name = std::move(*name);
            }
            break;
        }
        case ActionType::SetShowRemoteCallout: {
            if (auto val = action.get_payload<bool>()) next.show_remote_callout = *val;
            break;
        }

        // ========================================
        // Remote
        // ========================================
        case ActionType::SetRemoteSessionUrl: {
            if (auto url = action.get_payload<std::optional<std::string>>()) {
                next.remote_session_url = std::move(*url);
            }
            break;
        }
        case ActionType::SetRemoteConnectionStatus: {
            if (auto status = action.get_payload<RemoteConnectionStatus>()) {
                next.remote_connection_status = *status;
            }
            break;
        }
        case ActionType::SetRemoteBackgroundTaskCount: {
            if (auto count = action.get_payload<std::uint32_t>()) {
                next.remote_background_task_count = *count;
            }
            break;
        }

        // ========================================
        // Tasks and agents
        // ========================================
        case ActionType::AddTask:
        case ActionType::UpdateTask: {
            if (auto task = action.get_payload<TaskState>()) {
                next.tasks[task->id] = std::move(*task);
            }
            break;
        }
        case ActionType::RemoveTask: {
            if (auto id = action.get_payload<std::string>()) {
                next.tasks.erase(*id);
                if (next.foregrounded_task_id == *id) next.foregrounded_task_id = std::nullopt;
                if (next.viewing_agent_task_id == *id) next.viewing_agent_task_id = std::nullopt;
            }
            break;
        }
        case ActionType::SetForegroundedTaskId: {
            if (auto id = action.get_payload<std::optional<std::string>>()) {
                next.foregrounded_task_id = std::move(*id);
            }
            break;
        }
        case ActionType::SetViewingAgentTaskId: {
            if (auto id = action.get_payload<std::optional<std::string>>()) {
                next.viewing_agent_task_id = std::move(*id);
            }
            break;
        }
        case ActionType::RegisterAgentName: {
            if (auto entry = action.get_payload<std::pair<std::string, std::string>>()) {
                next.agent_name_registry[std::move(entry->first)] = std::move(entry->second);
            }
            break;
        }
        case ActionType::SetAgent: {
            if (auto agent = action.get_payload<std::optional<std::string>>()) {
                next.agent = std::move(*agent);
            }
            break;
        }
        case ActionType::SetKairosEnabled: {
            if (auto val = action.get_payload<bool>()) {
                next.kairos_enabled = *val;
            }
            break;
        }

        // ========================================
        // Companion
        // ========================================
        case ActionType::SetCompanionReaction: {
            if (auto reaction = action.get_payload<std::optional<std::string>>()) {
                next.companion_reaction = std::move(*reaction);
            }
            break;
        }
        case ActionType::SetCompanionPetTime: {
            if (auto time = action.get_payload<std::optional<std::chrono::system_clock::time_point>>()) {
                next.companion_pet_at = *time;
            }
            break;
        }

        // ========================================
        // MCP and plugins
        // ========================================
        case ActionType::UpdateMcpState: {
            if (auto mcp = action.get_payload<MCPState>()) {
                next.mcp = std::move(*mcp);
            }
            break;
        }
        case ActionType::IncrementMcpReconnectKey: {
            ++next.mcp.plugin_reconnect_key;
            break;
        }
        case ActionType::UpdatePluginsState: {
            if (auto plugins = action.get_payload<AppState::PluginsState>()) {
                next.plugins = std::move(*plugins);
            }
            break;
        }
        case ActionType::SetPluginsNeedRefresh: {
            if (auto val = action.get_payload<bool>()) {
                next.plugins.needs_refresh = *val;
            }
            break;
        }

        // ========================================
        // Speculation and skill improvement
        // ========================================
        case ActionType::SetSpeculationState: {
            if (auto speculation = action.get_payload<SpeculationState>()) {
                next.speculation = std::move(*speculation);
            }
            break;
        }
        case ActionType::SetSpeculationTimeSaved: {
            if (auto time_saved = action.get_payload<std::int64_t>()) {
                next.speculation_session_time_saved_ms = *time_saved;
            }
            break;
        }
        case ActionType::SetSkillSuggestion: {
            if (auto suggestion = action.get_payload<std::optional<AppState::SkillImprovementState::Suggestion>>()) {
                next.skill_improvement.suggestion = std::move(*suggestion);
            }
            break;
        }

        // ========================================
        // Auth, initial message, overlays, advisor, Ultraplan
        // ========================================
        case ActionType::IncrementAuthVersion: {
            ++next.auth_version;
            break;
        }
        case ActionType::SetInitialMessage: {
            if (auto message = action.get_payload<AppState::InitialMessage>()) {
                next.initial_message = std::move(*message);
            }
            break;
        }
        case ActionType::ClearInitialMessage: {
            next.initial_message = std::nullopt;
            break;
        }
        case ActionType::SetEffortValue: {
            if (auto value = action.get_payload<std::optional<std::string>>()) {
                next.effort_value = std::move(*value);
            }
            break;
        }
        case ActionType::AddActiveOverlay: {
            if (auto overlay = action.get_payload<std::string>()) {
                next.active_overlays.insert(std::move(*overlay));
            }
            break;
        }
        case ActionType::RemoveActiveOverlay: {
            if (auto overlay = action.get_payload<std::string>()) {
                next.active_overlays.erase(*overlay);
            }
            break;
        }
        case ActionType::ClearActiveOverlays: {
            next.active_overlays.clear();
            break;
        }
        case ActionType::SetAdvisorModel: {
            if (auto model = action.get_payload<std::optional<std::string>>()) {
                next.advisor_model = std::move(*model);
            }
            break;
        }
        case ActionType::SetUltraplanLaunching: {
            if (auto val = action.get_payload<bool>()) next.ultraplan_launching = *val;
            break;
        }
        case ActionType::SetUltraplanSessionUrl: {
            if (auto url = action.get_payload<std::optional<std::string>>()) {
                next.ultraplan_session_url = std::move(*url);
            }
            break;
        }
        case ActionType::SetUltraplanPendingChoice: {
            if (auto choice = action.get_payload<std::optional<AppState::UltraplanPendingChoice>>()) {
                next.ultraplan_pending_choice = std::move(*choice);
            }
            break;
        }
        case ActionType::SetUltraplanLaunchPending: {
            if (auto pending = action.get_payload<std::optional<AppState::UltraplanLaunchPending>>()) {
                next.ultraplan_launch_pending = std::move(*pending);
            }
            break;
        }
        case ActionType::SetUltraplanMode: {
            if (auto val = action.get_payload<bool>()) next.is_ultraplan_mode = *val;
            break;
        }

        // ========================================
        // Worker sandbox, prompt suggestion, inbox
        // ========================================
        case ActionType::AddSandboxPermissionRequest: {
            if (auto request = action.get_payload<AppState::WorkerSandboxPermissions::PermissionRequest>()) {
                next.worker_sandbox_permissions.queue.push_back(std::move(*request));
            }
            break;
        }
        case ActionType::RemoveSandboxPermissionRequest: {
            if (auto id = action.get_payload<std::string>()) {
                std::erase_if(next.worker_sandbox_permissions.queue, [&](const auto& request) {
                    return request.request_id == *id;
                });
            }
            break;
        }
        case ActionType::SetSelectedSandboxPermissionIndex: {
            if (auto index = action.get_payload<std::size_t>()) {
                next.worker_sandbox_permissions.selected_index = *index;
            }
            break;
        }
        case ActionType::SetPendingWorkerRequest: {
            if (auto request = action.get_payload<std::optional<AppState::PendingWorkerRequest>>()) {
                next.pending_worker_request = std::move(*request);
            }
            break;
        }
        case ActionType::SetPendingSandboxRequest: {
            if (auto request = action.get_payload<std::optional<AppState::PendingSandboxRequest>>()) {
                next.pending_sandbox_request = std::move(*request);
            }
            break;
        }
        case ActionType::SetPromptSuggestion: {
            if (auto suggestion = action.get_payload<AppState::PromptSuggestionState>()) {
                next.prompt_suggestion = std::move(*suggestion);
            }
            break;
        }
        case ActionType::ClearPromptSuggestion: {
            next.prompt_suggestion = AppState::PromptSuggestionState{};
            break;
        }
        case ActionType::AddInboxMessage: {
            if (auto message = action.get_payload<AppState::InboxState::InboxMessage>()) {
                next.inbox.messages.push_back(std::move(*message));
            }
            break;
        }
        case ActionType::RemoveInboxMessage: {
            if (auto id = action.get_payload<std::string>()) {
                std::erase_if(next.inbox.messages, [&](const auto& message) {
                    return message.id == *id;
                });
            }
            break;
        }
        case ActionType::ClearInboxMessages: {
            next.inbox.messages.clear();
            break;
        }

        // ========================================
        // Batch actions
        // ========================================
        case ActionType::Batch: {
            if (auto actions = action.get_payload<std::vector<Action>>()) {
                AppState batch_state = state;
                for (const auto& batch_action : *actions) {
                    batch_state = app_reducer(batch_state, batch_action);
                }
                return batch_state;
            }
            break;
        }

        // Tool and persistence actions require behavior outside pure AppState,
        // so the reducer deliberately leaves them as no-ops until those effects
        // have explicit payload and service semantics.
        case ActionType::EnableTool:
        case ActionType::DisableTool:
        case ActionType::SaveState:
        case ActionType::LoadState:
        case ActionType::ClearSavedState:
            break;

        default:
            break;
    }

    return next;
}

// ============================================================
// Middleware
// ============================================================

/// Middleware type: wraps dispatch to intercept/transform actions
using DispatchFn = std::function<void(const Action&)>;
using Middleware = std::function<DispatchFn(DispatchFn next)>;

/// Logging middleware: prints dispatched actions (for debug)
[[nodiscard]] inline Middleware logging_middleware() {
    return [](DispatchFn next) -> DispatchFn {
        return [next = std::move(next)](const Action& action) {
            // In production, log to file or structured logger
            next(action);
        };
    };
}

/// Persistence middleware.
///
/// NOTE: this is an intentional no-op. The TS reference (src/state/store.ts)
/// does not implement a middleware pipeline at all — its Store is a trivial
/// getState/setState/subscribe object, and persistence of the few AppState
/// fields the TS app actually persists happens through targeted global-config
/// writes in src/state/onChangeAppState.ts, not through a store middleware.
///
/// In this C++ port persistence is handled directly by the Store in
/// notify() (the AppState-typed auto-persist branch below) via the
/// StatePersistence manager, which is the equivalent of the TS onChange
/// callback path. This middleware therefore exists only for API parity and
/// deliberately performs no work; if per-action persistence hooks are ever
/// needed, dispatch them here.
[[nodiscard]] inline Middleware persistence_middleware(
    std::shared_ptr<persistence::StatePersistence> /*persistence*/
) {
    return [](DispatchFn next) -> DispatchFn {
        return [next = std::move(next)](const Action& action) {
            next(action);
        };
    };
}

// ============================================================
// Thunk (Async Action Support)
// ============================================================

/// Thunk type: an async action that receives dispatch + getState
using GetStateFn = std::function<AppState()>;
using Thunk = std::function<void(DispatchFn dispatch, GetStateFn get_state)>;

// ============================================================
// Store Class Template
// ============================================================

/// Enhanced Redux-like state store with middleware pipeline and subscriptions
template <typename State, Reducer<State> ReducerFn>
class Store {
    State state_;
    ReducerFn reducer_;
    mutable std::shared_mutex mutex_;

    // State-typed observer for this Store instance (decouples the subscriber
    // type from the AppState-specific StateObserver alias in app_state.cppm).
    using Observer = std::function<void(const State&, const State&)>;

    // Subscription management
    std::unordered_map<SubscriptionId, Observer> subscribers_;
    SubscriptionId next_sub_id_{1};

    // Middleware chain
    std::vector<Middleware> middlewares_;
    DispatchFn dispatch_chain_;

    // State change callback integration
    std::shared_ptr<on_change::StateChangeRegistry> change_registry_;

    // Persistence integration
    std::shared_ptr<persistence::StatePersistence> persistence_;
    std::chrono::steady_clock::time_point last_persist_time_;
    static constexpr auto AUTO_PERSIST_INTERVAL = std::chrono::seconds(5);

    // Undo/redo (opt-in state-snapshot history). Disabled by default so the
    // default construction path is unchanged; enable via enable_undo().
    bool undo_enabled_ = false;
    std::size_t undo_capacity_ = 0;
    std::vector<State> undo_stack_;
    std::vector<State> redo_stack_;
    static constexpr std::size_t kDefaultUndoCapacity = 100;

public:
    /// Construct store with initial state and reducer
    explicit Store(
        State initial,
        ReducerFn reducer,
        std::shared_ptr<on_change::StateChangeRegistry> registry = nullptr,
        std::shared_ptr<persistence::StatePersistence> persistence = nullptr
    ) : state_(std::move(initial)),
        reducer_(std::move(reducer)),
        change_registry_(std::move(registry)),
        persistence_(std::move(persistence)) {
        rebuild_dispatch_chain();
    }

    /// Get current state snapshot (thread-safe)
    [[nodiscard]] State get_state() const {
        std::shared_lock lock(mutex_);
        return state_;
    }

    /// Dispatch a synchronous action through the middleware chain
    void dispatch(const Action& action) {
        dispatch_chain_(action);
    }

    /// Dispatch a thunk (async action)
    void dispatch_thunk(Thunk thunk) {
        auto dispatch_fn = [this](const Action& a) { dispatch(a); };
        auto get_state_fn = [this]() { return get_state(); };
        thunk(dispatch_fn, get_state_fn);
    }

    /// Add middleware (must call before first dispatch for correct ordering)
    void add_middleware(Middleware mw) {
        std::unique_lock lock(mutex_);
        middlewares_.push_back(std::move(mw));
        rebuild_dispatch_chain();
    }

    /// Subscribe to state changes
    [[nodiscard]] SubscriptionId subscribe(Observer observer) {
        std::unique_lock lock(mutex_);
        auto id = next_sub_id_++;
        subscribers_.emplace(id, std::move(observer));
        return id;
    }

    /// Unsubscribe from state changes
    void unsubscribe(SubscriptionId id) {
        std::unique_lock lock(mutex_);
        subscribers_.erase(id);
    }

    /// Set the state change registry
    void set_change_registry(std::shared_ptr<on_change::StateChangeRegistry> registry) {
        std::unique_lock lock(mutex_);
        change_registry_ = std::move(registry);
    }

    /// Set the persistence manager
    void set_persistence(std::shared_ptr<persistence::StatePersistence> persistence) {
        std::unique_lock lock(mutex_);
        persistence_ = std::move(persistence);
    }

    // ── Undo / redo (state-snapshot based) ────────────────────────────────
    // Each dispatched action pushes the pre-action state onto the undo stack
    // and clears the redo stack (standard model). Snapshots are bounded by the
    // configured capacity; the oldest is dropped first.

    /// Enable undo/redo history with the given snapshot capacity (min 1).
    void enable_undo(std::size_t capacity = kDefaultUndoCapacity) {
        std::unique_lock lock(mutex_);
        undo_enabled_ = true;
        undo_capacity_ = capacity < 1 ? 1 : capacity;
        undo_stack_.clear();
        redo_stack_.clear();
    }

    /// Disable undo/redo history and discard all snapshots.
    void disable_undo() {
        std::unique_lock lock(mutex_);
        undo_enabled_ = false;
        undo_stack_.clear();
        redo_stack_.clear();
    }

    [[nodiscard]] bool undo_enabled() const {
        std::shared_lock lock(mutex_);
        return undo_enabled_;
    }

    [[nodiscard]] bool can_undo() const {
        std::shared_lock lock(mutex_);
        return undo_enabled_ && !undo_stack_.empty();
    }

    [[nodiscard]] bool can_redo() const {
        std::shared_lock lock(mutex_);
        return undo_enabled_ && !redo_stack_.empty();
    }

    /// Revert to the previous state snapshot. No-op if history is disabled/empty.
    void undo() {
        State prev;
        State next;
        {
            std::unique_lock lock(mutex_);
            if (!undo_enabled_ || undo_stack_.empty()) return;
            redo_stack_.push_back(state_);
            if (redo_stack_.size() > undo_capacity_) redo_stack_.erase(redo_stack_.begin());
            state_ = std::move(undo_stack_.back());
            undo_stack_.pop_back();
            prev = redo_stack_.back();
            next = state_;
        }
        notify(prev, next);
    }

    /// Re-apply a state that was undone. No-op if there is nothing to redo.
    void redo() {
        State prev;
        State next;
        {
            std::unique_lock lock(mutex_);
            if (!undo_enabled_ || redo_stack_.empty()) return;
            undo_stack_.push_back(state_);
            state_ = std::move(redo_stack_.back());
            redo_stack_.pop_back();
            prev = undo_stack_.back();
            next = state_;
        }
        notify(prev, next);
    }

    /// Discard all undo/redo history (the feature stays enabled).
    void clear_history() {
        std::unique_lock lock(mutex_);
        undo_stack_.clear();
        redo_stack_.clear();
    }

    /// Manually save the current state
    [[nodiscard]] cc::core::VoidResult save_state() {
        if (!persistence_) {
            return {}; // No persistence configured
        }
        auto saved = persistence_->save_state(get_state());
        if (!saved) {
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::InternalError,
                saved.error().format()));
        }
        return {};
    }

    /// Load state from persistence
    [[nodiscard]] std::expected<bool, cc::core::Error> load_state() {
        if (!persistence_) {
            return false; // No persistence configured
        }
        auto loaded = persistence_->load_state();
        if (!loaded) {
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::InternalError,
                loaded.error().format()));
        }
        // Update state with loaded value
        State prev;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = std::move(*loaded);
        }
        notify(prev, state_);
        return true;
    }

private:
    /// Core dispatch: apply reducer and notify subscribers
    void core_dispatch(const Action& action) {
        State prev;
        State next;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = reducer_(state_, action);
            next = state_;
            if (undo_enabled_) {
                push_undo_locked(prev);
                redo_stack_.clear();
            }
        }
        notify(prev, next);
    }

    /// Push a pre-action snapshot onto the undo stack, dropping the oldest
    /// entry when the capacity is exceeded. Caller holds mutex_.
    void push_undo_locked(const State& snapshot) {
        undo_stack_.push_back(snapshot);
        if (undo_stack_.size() > undo_capacity_) {
            undo_stack_.erase(undo_stack_.begin());
        }
    }

    /// Rebuild middleware pipeline after adding new middleware
    void rebuild_dispatch_chain() {
        // Start from core dispatch
        DispatchFn chain = [this](const Action& a) { core_dispatch(a); };
        // Apply middlewares in reverse order (outermost first)
        for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
            chain = (*it)(std::move(chain));
        }
        dispatch_chain_ = std::move(chain);
    }

    /// Notify all subscribers and change registry
    void notify(const State& prev, const State& next) {
        // Notify subscribers
        std::vector<Observer> observers;
        {
            std::shared_lock lock(mutex_);
            observers.reserve(subscribers_.size());
            for (const auto& [_, ob] : subscribers_) {
                observers.push_back(ob);
            }
        }
        for (const auto& ob : observers) {
            ob(prev, next);
        }

        // The change-registry and auto-persist hooks are AppState-specific
        // (they consume `const AppState&`). Guard them so the Store template
        // remains instantiable with any State — generic consumers still get
        // subscriber notifications, undo/redo, and middleware.
        if constexpr (std::is_same_v<State, AppState>) {
            // Notify change registry
            if (change_registry_) {
                change_registry_->run_callbacks(prev, next);
            }

            // Auto-persist if needed
            if (persistence_ && persistence_->is_auto_save_enabled()) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_persist_time_ >= AUTO_PERSIST_INTERVAL) {
                    auto result = persistence_->save_state(next);
                    if (result) {
                        last_persist_time_ = now;
                    }
                }
            }
        }
    }
};

// ============================================================
// Convenience Type Alias and Factory
// ============================================================

/// The application store type with default reducer
using AppStore = Store<AppState, decltype(&app_reducer)>;

/// Factory function to create the application store with full setup
[[nodiscard]] inline std::unique_ptr<AppStore> create_app_store(
    AppState initial = get_default_app_state()
) {
    // Create change registry and set up default handlers
    auto registry = std::make_shared<on_change::StateChangeRegistry>();
    on_change::setup_default_handlers(*registry);

    // Create persistence manager
    auto persistence = std::make_shared<persistence::StatePersistence>(
        persistence::get_default_state_file_path()
    );

    // Create store with all integrations
    auto store = std::make_unique<AppStore>(
        std::move(initial),
        &app_reducer,
        registry,
        persistence
    );

    // Add logging middleware (optional, can be conditional)
    store->add_middleware(logging_middleware());

    return store;
}

} // namespace cc::state
