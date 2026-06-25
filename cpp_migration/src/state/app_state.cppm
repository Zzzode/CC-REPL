/// @file app_state.cppm
/// @brief Application state module for the Claude Code REPL.
/// Defines the full AppState struct, immutable update functions,
/// observer pattern for reactive state changes, and selectors.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <algorithm>
#include <ranges>
#include <concepts>
#include <variant>
#include <map>
#include <set>

export module cc.state.app_state;

import cc.types.types;

export namespace cc::state {

// ============================================================
// Permission State
// ============================================================

/// Permission mode enum
enum class PermissionMode : std::uint8_t {
    Default,
    Plan,
    Auto,
    Bubble,
    UngatedAuto,
};

/// Convert PermissionMode to string
[[nodiscard]] constexpr std::string_view permission_mode_to_string(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Default: return "default";
        case PermissionMode::Plan: return "plan";
        case PermissionMode::Auto: return "auto";
        case PermissionMode::Bubble: return "bubble";
        case PermissionMode::UngatedAuto: return "ungated-auto";
    }
    return "unknown";
}

/// Tool permission context
struct ToolPermissionContext {
    PermissionMode mode = PermissionMode::Default;
    std::unordered_set<std::string> allowed_tools;
    std::unordered_set<std::string> denied_tools;
};

// ============================================================
// Settings Type Definitions
// ============================================================


/// Status line settings (mirrors TS settings.statusLine)
struct StatusLineSettings {
    bool enabled = false;
    std::string command;        // Shell command whose output is shown in the status line
    int padding = 0;            // Horizontal padding (TS: paddingX)
};

struct Settings {
    std::string model;
    std::string theme;
    bool verbose = false;
    std::string output_style;   // TS: settings.outputStyle
    bool disable_all_hooks = false;
    StatusLineSettings status_line;
    std::unordered_map<std::string, std::string> env_vars;
    // Add other settings fields...
};

/// Completion boundary
enum class CompletionBoundaryType {
    Complete,
    Bash,
    Edit,
    DeniedTool,
};

struct CompletionBoundary {
    CompletionBoundaryType type;
    std::optional<std::uint64_t> completed_at;
    std::optional<std::uint32_t> output_tokens;
    std::optional<std::string> command;
    std::optional<std::string> tool_name;
    std::optional<std::string> file_path;
    std::optional<std::string> detail;
};

/// Speculation state
struct SpeculationResult {
    std::vector<cc::core::Message> messages;
    std::optional<CompletionBoundary> boundary;
    std::int64_t time_saved_ms = 0;
};

enum class SpeculationStatus {
    Idle,
    Active,
};

struct SpeculationState {
    SpeculationStatus status = SpeculationStatus::Idle;
    std::string id;
    std::optional<std::function<void()>> abort;
    std::chrono::system_clock::time_point start_time;
    std::vector<cc::core::Message> messages;
    std::set<std::string> written_paths;
    std::optional<CompletionBoundary> boundary;
    std::size_t suggestion_length = 0;
    std::size_t tool_use_count = 0;
    bool is_pipelined = false;
    // Add more fields as needed...
};

/// Footer item
enum class FooterItem {
    Tasks,
    Tmux,
    Bagel,
    Teams,
    Bridge,
    Companion,
};

/// Expanded view
enum class ExpandedView {
    None,
    Tasks,
    Teammates,
};

/// Remote connection status
enum class RemoteConnectionStatus {
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
};

// ============================================================
// Model Configuration
// ============================================================

/// Configuration for the active model
struct ModelConfig {
    std::string model_id;           // e.g., "claude-sonnet-4-20250514"
    std::string display_name;       // e.g., "Claude Sonnet 4"
    std::uint32_t max_tokens = 8192;
    double temperature = 1.0;
    std::optional<std::string> system_prompt_override;
    double input_cost_per_mtok = 3.0;   // $ per million input tokens
    double output_cost_per_mtok = 15.0; // $ per million output tokens
};

// ============================================================
// Plugin State
// ============================================================

/// Plugin error
struct PluginError {
    std::string plugin_id;
    std::string message;
    std::optional<std::string> detail;
};

/// Plugin installation status
struct InstallationStatus {
    struct MarketplaceStatus {
        std::string name;
        enum class Status { Pending, Installing, Installed, Failed } status;
        std::optional<std::string> error;
    };
    struct PluginStatus {
        std::string id;
        std::string name;
        enum class Status { Pending, Installing, Installed, Failed } status;
        std::optional<std::string> error;
    };
    std::vector<MarketplaceStatus> marketplaces;
    std::vector<PluginStatus> plugins;
};

struct LoadedPlugin {
    std::string id;
    std::string name;
    std::string version;
    bool enabled = false;
    std::vector<std::string> commands;
    std::vector<std::string> tools;
};

// ============================================================
// MCP State
// ============================================================

struct MCPServerConnection {
    std::string id;
    std::string name;
    std::string url;
    bool connected = false;
};

struct MCPTool {
    std::string name;
    std::string description;
    std::string input_schema;
};

struct MCPCommand {
    std::string name;
    std::string description;
};

struct MCPServerResource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

struct MCPState {
    std::vector<MCPServerConnection> clients;
    std::vector<MCPTool> tools;
    std::vector<MCPCommand> commands;
    std::unordered_map<std::string, std::vector<MCPServerResource>> resources;
    std::uint32_t plugin_reconnect_key = 0;
};

// ============================================================
// Task State (simplified)
// ============================================================

struct TaskState {
    std::string id;
    std::string title;
    std::string status;
    std::vector<cc::core::Message> messages;
    std::chrono::system_clock::time_point created_at;
    // Add more fields as needed...
};

// ============================================================
// AppState - Full application state
// ============================================================

/// Immutable application state snapshot
struct AppState {
    // ========================================
    // Settings & Configuration
    // ========================================
    Settings settings;
    bool verbose = false;
    std::optional<std::string> main_loop_model;
    std::optional<std::string> main_loop_model_for_session;
    std::optional<std::string> status_line_text;
    ExpandedView expanded_view = ExpandedView::None;
    bool is_brief_only = false;
    bool show_teammate_message_preview = false;
    std::int32_t selected_ip_agent_index = -1;
    std::int32_t coordinator_task_index = -1;
    std::string view_selection_mode = "none";
    std::optional<FooterItem> footer_selection;
    ToolPermissionContext tool_permission_context;
    std::optional<std::string> spinner_tip;
    std::optional<std::string> agent;
    bool kairos_enabled = false;
    std::optional<std::string> remote_session_url;
    RemoteConnectionStatus remote_connection_status = RemoteConnectionStatus::Connecting;
    std::uint32_t remote_background_task_count = 0;

    // ========================================
    // Bridge State
    // ========================================
    bool repl_bridge_enabled = false;
    bool repl_bridge_explicit = false;
    bool repl_bridge_outbound_only = false;
    bool repl_bridge_connected = false;
    bool repl_bridge_session_active = false;
    bool repl_bridge_reconnecting = false;
    std::optional<std::string> repl_bridge_connect_url;
    std::optional<std::string> repl_bridge_session_url;
    std::optional<std::string> repl_bridge_environment_id;
    std::optional<std::string> repl_bridge_session_id;
    std::optional<std::string> repl_bridge_error;
    std::optional<std::string> repl_bridge_initial_name;
    bool show_remote_callout = false;

    // ========================================
    // Tasks & Agents
    // ========================================
    std::unordered_map<std::string, TaskState> tasks;
    std::unordered_map<std::string, std::string> agent_name_registry;
    std::optional<std::string> foregrounded_task_id;
    std::optional<std::string> viewing_agent_task_id;

    // ========================================
    // Companion (Buddy)
    // ========================================
    std::optional<std::string> companion_reaction;
    std::optional<std::chrono::system_clock::time_point> companion_pet_at;

    // ========================================
    // MCP & Plugins
    // ========================================
    MCPState mcp;
    struct PluginsState {
        std::vector<LoadedPlugin> enabled;
        std::vector<LoadedPlugin> disabled;
        std::vector<std::string> commands;
        std::vector<PluginError> errors;
        InstallationStatus installation_status;
        bool needs_refresh = false;
    } plugins;

    // ========================================
    // Conversation & Messages
    // ========================================
    std::vector<cc::core::Message> messages;
    cc::core::SessionId session_id;
    cc::core::ConversationId conversation_id;

    // ========================================
    // Model Configuration
    // ========================================
    ModelConfig current_model;
    std::vector<ModelConfig> available_models;

    // ========================================
    // Status Flags
    // ========================================
    bool is_loading = false;
    bool is_streaming = false;
    std::optional<std::string> error_message;

    // ========================================
    // Token & Cost Tracking
    // ========================================
    cc::core::TokenUsage total_usage;
    double total_cost_usd = 0.0;

    // ========================================
    // UI State
    // ========================================
    bool compact_mode = false;
    bool show_thinking = false;
    std::optional<std::string> active_slash_command;
    std::vector<std::string> notifications;

    // ========================================
    // Session Metadata
    // ========================================
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_activity;
    std::string working_directory;

    // ========================================
    // Additional State Fields
    // ========================================
    // ... Add more fields as needed from TypeScript version
    bool thinking_enabled = true;
    bool prompt_suggestion_enabled = true;
    std::map<std::string, std::string> session_hooks;
    struct InboxState {
        struct InboxMessage {
            std::string id;
            std::string from;
            std::string text;
            std::string timestamp;
            std::string status;
            std::optional<std::string> color;
            std::optional<std::string> summary;
        };
        std::vector<InboxMessage> messages;
    } inbox;
    struct WorkerSandboxPermissions {
        struct PermissionRequest {
            std::string request_id;
            std::string worker_id;
            std::string worker_name;
            std::optional<std::string> worker_color;
            std::string host;
            std::chrono::system_clock::time_point created_at;
        };
        std::vector<PermissionRequest> queue;
        std::size_t selected_index = 0;
    } worker_sandbox_permissions;
    struct PendingWorkerRequest {
        std::string tool_name;
        std::string tool_use_id;
        std::string description;
    };
    std::optional<PendingWorkerRequest> pending_worker_request;
    struct PendingSandboxRequest {
        std::string request_id;
        std::string host;
    };
    std::optional<PendingSandboxRequest> pending_sandbox_request;
    struct PromptSuggestionState {
        std::optional<std::string> text;
        std::optional<std::string> prompt_id;
        std::chrono::system_clock::time_point shown_at;
        std::chrono::system_clock::time_point accepted_at;
        std::optional<std::string> generation_request_id;
    } prompt_suggestion;
    SpeculationState speculation;
    std::int64_t speculation_session_time_saved_ms = 0;
    struct SkillImprovementState {
        struct Suggestion {
            std::string skill_name;
            std::vector<std::tuple<std::string, std::string, std::string>> updates; // section, change, reason
        };
        std::optional<Suggestion> suggestion;
    } skill_improvement;
    std::uint32_t auth_version = 0;
    struct InitialMessage {
        cc::core::UserMessage message;
        bool clear_context = false;
        std::optional<PermissionMode> mode;
        std::vector<std::string> allowed_prompts;
    };
    std::optional<InitialMessage> initial_message;
    std::optional<std::string> effort_value;
    std::set<std::string> active_overlays;
    bool fast_mode = false;
    std::optional<std::string> advisor_model;
    bool ultraplan_launching = false;
    std::optional<std::string> ultraplan_session_url;
    struct UltraplanPendingChoice {
        std::string plan;
        std::string session_id;
        std::string task_id;
    };
    std::optional<UltraplanPendingChoice> ultraplan_pending_choice;
    struct UltraplanLaunchPending {
        std::string blurb;
    };
    std::optional<UltraplanLaunchPending> ultraplan_launch_pending;
    bool is_ultraplan_mode = false;
};

// ============================================================
// Get Default AppState
// ============================================================

/// Get default application state
[[nodiscard]] inline AppState get_default_app_state() {
    AppState state;
    state.created_at = std::chrono::system_clock::now();
    state.last_activity = std::chrono::system_clock::now();
    return state;
}

// ============================================================
// Immutable State Update Functions
// ============================================================

/// Create a new state with an appended message
[[nodiscard]] inline AppState with_message(const AppState& state, cc::core::Message msg) {
    auto next = state;
    next.messages.push_back(std::move(msg));
    next.last_activity = std::chrono::system_clock::now();
    return next;
}

/// Create a new state with loading flag toggled
[[nodiscard]] inline AppState with_loading(const AppState& state, bool loading) {
    auto next = state;
    next.is_loading = loading;
    next.is_streaming = loading;
    return next;
}

/// Create a new state with updated token usage
[[nodiscard]] inline AppState with_usage(const AppState& state, cc::core::TokenUsage usage) {
    auto next = state;
    next.total_usage += usage;
    double input_cost = static_cast<double>(usage.input_tokens) * state.current_model.input_cost_per_mtok / 1'000'000.0;
    double output_cost = static_cast<double>(usage.output_tokens) * state.current_model.output_cost_per_mtok / 1'000'000.0;
    next.total_cost_usd += input_cost + output_cost;
    return next;
}

/// Create a new state with error set
[[nodiscard]] inline AppState with_error(const AppState& state, std::string error) {
    auto next = state;
    next.error_message = std::move(error);
    next.is_loading = false;
    next.is_streaming = false;
    return next;
}

/// Create a new state with error cleared
[[nodiscard]] inline AppState clear_error(const AppState& state) {
    auto next = state;
    next.error_message = std::nullopt;
    return next;
}

/// Create a new state with model switched
[[nodiscard]] inline AppState with_model(const AppState& state, ModelConfig model) {
    auto next = state;
    next.current_model = std::move(model);
    return next;
}

/// Create a new state with verbose toggled
[[nodiscard]] inline AppState with_verbose(const AppState& state, bool verbose) {
    auto next = state;
    next.verbose = verbose;
    return next;
}

/// Create a new state with expanded view changed
[[nodiscard]] inline AppState with_expanded_view(const AppState& state, ExpandedView view) {
    auto next = state;
    next.expanded_view = view;
    return next;
}

/// Create a new state with permission mode changed
[[nodiscard]] inline AppState with_permission_mode(const AppState& state, PermissionMode mode) {
    auto next = state;
    next.tool_permission_context.mode = mode;
    return next;
}

// ============================================================
// Observer Pattern - reactive state change notifications
// ============================================================

/// Subscription ID for unsubscribing
using SubscriptionId = std::uint64_t;

/// Callback type for state change observers
using StateObserver = std::function<void(const AppState& prev, const AppState& next)>;

/// Observable state container with change notification
class ObservableState {
    AppState state_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<SubscriptionId, StateObserver> observers_;
    SubscriptionId next_id_{1};

public:
    explicit ObservableState(AppState initial = get_default_app_state())
        : state_(std::move(initial)) {}

    /// Get a snapshot of current state (thread-safe)
    [[nodiscard]] AppState get() const {
        std::shared_lock lock(mutex_);
        return state_;
    }

    /// Apply an update function and notify observers
    template <typename Fn>
        requires std::invocable<Fn, const AppState&> &&
                 std::same_as<std::invoke_result_t<Fn, const AppState&>, AppState>
    void update(Fn&& updater) {
        AppState prev, next;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = std::forward<Fn>(updater)(state_);
            next = state_;
        }
        notify_observers(prev, next);
    }

    /// Set state directly and notify observers
    void set(const AppState& new_state) {
        AppState prev;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = new_state;
        }
        notify_observers(prev, new_state);
    }

    /// Subscribe to state changes, returns ID for unsubscribing
    [[nodiscard]] SubscriptionId subscribe(StateObserver observer) {
        std::unique_lock lock(mutex_);
        auto id = next_id_++;
        observers_.emplace(id, std::move(observer));
        return id;
    }

    /// Unsubscribe from state changes
    void unsubscribe(SubscriptionId id) {
        std::unique_lock lock(mutex_);
        observers_.erase(id);
    }

private:
    void notify_observers(const AppState& prev, const AppState& next) {
        std::vector<StateObserver> observers;
        {
            std::shared_lock lock(mutex_);
            observers.reserve(observers_.size());
            for (const auto& [_, ob] : observers_) {
                observers.push_back(ob);
            }
        }
        for (const auto& ob : observers) {
            ob(prev, next);
        }
    }
};

} // namespace cc::state
