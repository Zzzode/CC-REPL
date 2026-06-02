// Hooks Registry Module
// Merges: AsyncHookRegistry, hookEvents, hookHelpers, hooksSettings
// Provides core hook registration, event broadcasting, and settings resolution
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.utils.hooks_registry;

import cc.utils.json;
import cc.utils.async;

export namespace cc::utils::hooks_registry {

// =========================================================================
// Hook Event Types (from hookEvents.ts)
// =========================================================================

/// All possible hook event names matching the TS HookEvent union type
enum class HookEventType {
    PreToolUse,
    PostToolUse,
    PostToolUseFailure,
    PermissionDenied,
    Notification,
    UserPromptSubmit,
    SessionStart,
    SessionEnd,
    Stop,
    StopFailure,
    SubagentStart,
    SubagentStop,
    PreCompact,
    PostCompact,
    PermissionRequest,
    Setup,
    TeammateIdle,
    TaskCreated,
    TaskCompleted,
    Elicitation,
    ElicitationResult,
    ConfigChange,
    InstructionsLoaded,
    WorktreeCreate,
    WorktreeRemove,
    CwdChanged,
    FileChanged,
};

/// Convert HookEventType to string_view
[[nodiscard]] constexpr std::string_view hook_event_name(HookEventType event) noexcept {
    switch (event) {
        case HookEventType::PreToolUse:          return "PreToolUse";
        case HookEventType::PostToolUse:         return "PostToolUse";
        case HookEventType::PostToolUseFailure:  return "PostToolUseFailure";
        case HookEventType::PermissionDenied:    return "PermissionDenied";
        case HookEventType::Notification:        return "Notification";
        case HookEventType::UserPromptSubmit:    return "UserPromptSubmit";
        case HookEventType::SessionStart:        return "SessionStart";
        case HookEventType::SessionEnd:          return "SessionEnd";
        case HookEventType::Stop:                return "Stop";
        case HookEventType::StopFailure:         return "StopFailure";
        case HookEventType::SubagentStart:       return "SubagentStart";
        case HookEventType::SubagentStop:        return "SubagentStop";
        case HookEventType::PreCompact:          return "PreCompact";
        case HookEventType::PostCompact:         return "PostCompact";
        case HookEventType::PermissionRequest:   return "PermissionRequest";
        case HookEventType::Setup:               return "Setup";
        case HookEventType::TeammateIdle:        return "TeammateIdle";
        case HookEventType::TaskCreated:         return "TaskCreated";
        case HookEventType::TaskCompleted:       return "TaskCompleted";
        case HookEventType::Elicitation:         return "Elicitation";
        case HookEventType::ElicitationResult:   return "ElicitationResult";
        case HookEventType::ConfigChange:        return "ConfigChange";
        case HookEventType::InstructionsLoaded:  return "InstructionsLoaded";
        case HookEventType::WorktreeCreate:      return "WorktreeCreate";
        case HookEventType::WorktreeRemove:      return "WorktreeRemove";
        case HookEventType::CwdChanged:          return "CwdChanged";
        case HookEventType::FileChanged:         return "FileChanged";
    }
    return "Unknown";
}

// =========================================================================
// Hook Execution Event (broadcast events for hook lifecycle)
// =========================================================================

/// Outcome of a hook execution
enum class HookOutcome {
    Success,
    Error,
    Cancelled,
};

/// Event emitted when a hook starts
struct HookStartedEvent {
    std::string hook_id;
    std::string hook_name;
    std::string hook_event;
};

/// Event emitted during hook progress
struct HookProgressEvent {
    std::string hook_id;
    std::string hook_name;
    std::string hook_event;
    std::string stdout_output;
    std::string stderr_output;
    std::string combined_output;
};

/// Event emitted when a hook completes
struct HookResponseEvent {
    std::string hook_id;
    std::string hook_name;
    std::string hook_event;
    std::string combined_output;
    std::string stdout_output;
    std::string stderr_output;
    std::optional<int> exit_code;
    HookOutcome outcome;
};

/// Discriminated union for all hook execution events
using HookExecutionEvent = std::variant<HookStartedEvent, HookProgressEvent, HookResponseEvent>;

/// Handler function type for hook execution events
using HookEventHandler = std::function<void(const HookExecutionEvent&)>;

// =========================================================================
// HookEvent - Event system for broadcasting hook lifecycle
// =========================================================================

/// Events that are always emitted regardless of configuration
inline constexpr std::array kAlwaysEmittedEvents = {
    HookEventType::SessionStart,
    HookEventType::Setup,
};

/// Maximum number of pending events before oldest are dropped
inline constexpr std::size_t kMaxPendingEvents = 100;

/// Hook event broadcasting system
class HookEvent {
public:
    /// Register a handler to receive hook events; flushes pending events
    void register_handler(HookEventHandler handler) {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
        if (handler_) {
            for (auto& event : pending_events_) {
                handler_(event);
            }
            pending_events_.clear();
        }
    }

    /// Remove the current handler
    void clear_handler() {
        std::lock_guard lock(mutex_);
        handler_ = nullptr;
    }

    /// Emit a hook started event
    void emit_started(std::string hook_id, std::string hook_name, std::string hook_event) {
        if (!should_emit(hook_event)) return;
        emit(HookStartedEvent{
            .hook_id = std::move(hook_id),
            .hook_name = std::move(hook_name),
            .hook_event = std::move(hook_event),
        });
    }

    /// Emit a hook progress event
    void emit_progress(std::string hook_id, std::string hook_name,
                       std::string hook_event, std::string stdout_out,
                       std::string stderr_out, std::string combined) {
        if (!should_emit(hook_event)) return;
        emit(HookProgressEvent{
            .hook_id = std::move(hook_id),
            .hook_name = std::move(hook_name),
            .hook_event = std::move(hook_event),
            .stdout_output = std::move(stdout_out),
            .stderr_output = std::move(stderr_out),
            .combined_output = std::move(combined),
        });
    }

    /// Emit a hook response (completion) event
    void emit_response(std::string hook_id, std::string hook_name,
                       std::string hook_event, std::string combined,
                       std::string stdout_out, std::string stderr_out,
                       std::optional<int> exit_code, HookOutcome outcome) {
        if (!should_emit(hook_event)) return;
        emit(HookResponseEvent{
            .hook_id = std::move(hook_id),
            .hook_name = std::move(hook_name),
            .hook_event = std::move(hook_event),
            .combined_output = std::move(combined),
            .stdout_output = std::move(stdout_out),
            .stderr_output = std::move(stderr_out),
            .exit_code = exit_code,
            .outcome = outcome,
        });
    }

    /// Enable or disable emission of all hook events (beyond always-emitted)
    void set_all_events_enabled(bool enabled) noexcept {
        all_events_enabled_.store(enabled, std::memory_order_relaxed);
    }

    /// Reset all state (for testing)
    void clear_state() {
        std::lock_guard lock(mutex_);
        handler_ = nullptr;
        pending_events_.clear();
        all_events_enabled_.store(false, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] bool should_emit(std::string_view hook_event) const noexcept {
        // Always emit certain events
        for (auto always : kAlwaysEmittedEvents) {
            if (hook_event_name(always) == hook_event) return true;
        }
        return all_events_enabled_.load(std::memory_order_relaxed);
    }

    void emit(HookExecutionEvent event) {
        std::lock_guard lock(mutex_);
        if (handler_) {
            handler_(event);
        } else {
            pending_events_.push_back(std::move(event));
            if (pending_events_.size() > kMaxPendingEvents) {
                pending_events_.erase(pending_events_.begin());
            }
        }
    }

    mutable std::mutex mutex_;
    HookEventHandler handler_;
    std::vector<HookExecutionEvent> pending_events_;
    std::atomic<bool> all_events_enabled_{false};
};

// =========================================================================
// Hook Command Types (from hooksSettings.ts / settings/types.ts)
// =========================================================================

/// Types of hook commands
enum class HookCommandType {
    Command,   // Shell command
    Prompt,    // LLM prompt hook
    Agent,     // Multi-turn LLM agent hook
    Http,      // HTTP webhook
    Function,  // In-process function hook
};

/// Shell command hook configuration
struct CommandHookConfig {
    std::string command;
    std::string shell = "bash";
    std::optional<std::string> condition; // "if" field
    std::optional<int> timeout_seconds;
};

/// Prompt-based hook configuration
struct PromptHookConfig {
    std::string prompt;
    std::optional<std::string> model;
    std::optional<std::string> condition;
    std::optional<int> timeout_seconds;
};

/// Agent hook configuration (multi-turn LLM)
struct AgentHookConfig {
    std::string prompt;
    std::optional<std::string> model;
    std::optional<std::string> condition;
    std::optional<int> timeout_seconds;
};

/// HTTP webhook configuration
struct HttpHookConfig {
    std::string url;
    std::optional<std::string> condition;
    std::optional<int> timeout_seconds;
};

/// Function (in-process callback) hook configuration
struct FunctionHookConfig {
    std::optional<std::string> status_message;
    std::optional<int> timeout_seconds;
};

/// Discriminated union for all hook command types
using HookCommand = std::variant<
    CommandHookConfig,
    PromptHookConfig,
    AgentHookConfig,
    HttpHookConfig,
    FunctionHookConfig
>;

/// Get the type of a hook command
[[nodiscard]] inline HookCommandType get_command_type(const HookCommand& cmd) noexcept {
    return static_cast<HookCommandType>(cmd.index());
}

// =========================================================================
// Hook Matcher (pattern matching for hook dispatch)
// =========================================================================

/// A matcher groups hooks by a pattern (e.g., tool name)
struct HookMatcher {
    std::optional<std::string> matcher; // Pattern string (empty = match all)
    std::vector<HookCommand> hooks;     // Hooks to execute when matched
};

/// Metadata about what field a matcher operates on
struct MatcherMetadata {
    std::string field_to_match;
    std::vector<std::string> values;
};

/// Metadata description for a hook event type
struct HookEventMetadata {
    std::string summary;
    std::string description;
    std::optional<MatcherMetadata> matcher_metadata;
};

/// Get metadata for a specific hook event type
[[nodiscard]] HookEventMetadata get_hook_event_metadata(
    HookEventType event,
    const std::vector<std::string>& tool_names);

// =========================================================================
// Hook Source (where a hook was defined)
// =========================================================================

enum class HookSource {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    PolicySettings,
    PluginHook,
    SessionHook,
    BuiltinHook,
};

/// Convert HookSource to display string
[[nodiscard]] inline std::string_view hook_source_inline_display(HookSource source) noexcept {
    switch (source) {
        case HookSource::UserSettings:    return "User";
        case HookSource::ProjectSettings: return "Project";
        case HookSource::LocalSettings:   return "Local";
        case HookSource::PolicySettings:  return "Policy";
        case HookSource::PluginHook:      return "Plugin";
        case HookSource::SessionHook:     return "Session";
        case HookSource::BuiltinHook:     return "Built-in";
    }
    return "Unknown";
}

/// Convert HookSource to description display string
[[nodiscard]] inline std::string hook_source_description(HookSource source) {
    switch (source) {
        case HookSource::UserSettings:    return "User settings (~/.claude/settings.json)";
        case HookSource::ProjectSettings: return "Project settings (.claude/settings.json)";
        case HookSource::LocalSettings:   return "Local settings (.claude/settings.local.json)";
        case HookSource::PolicySettings:  return "Policy settings (managed)";
        case HookSource::PluginHook:      return "Plugin hooks (~/.claude/plugins/*/hooks/hooks.json)";
        case HookSource::SessionHook:     return "Session hooks (in-memory, temporary)";
        case HookSource::BuiltinHook:     return "Built-in hooks (registered internally by Claude Code)";
    }
    return "Unknown source";
}

// =========================================================================
// IndividualHookConfig - fully resolved hook from any source
// =========================================================================

struct IndividualHookConfig {
    HookEventType event;
    HookCommand config;
    std::optional<std::string> matcher;
    HookSource source;
    std::optional<std::string> plugin_name;
};

// =========================================================================
// HookConfig - Aggregate configuration interface
// =========================================================================

/// Compare two hooks for equality (command/prompt content, not timeout)
[[nodiscard]] bool is_hook_equal(const HookCommand& a, const HookCommand& b) noexcept;

/// Get display text for a hook command
[[nodiscard]] std::string get_hook_display_text(const HookCommand& cmd);

/// Sort matchers by source priority
[[nodiscard]] std::vector<std::string> sort_matchers_by_priority(
    const std::vector<std::string>& matchers,
    const std::unordered_map<std::string, std::vector<IndividualHookConfig>>& hooks_by_matcher,
    HookEventType event);

// =========================================================================
// HookRegistry - Async hook lifecycle management (from AsyncHookRegistry.ts)
// =========================================================================

/// State of a pending asynchronous hook execution
struct PendingAsyncHook {
    std::string process_id;
    std::string hook_id;
    std::string hook_name;
    std::string hook_event;  // HookEventType name or "StatusLine"/"FileSuggestion"
    std::optional<std::string> tool_name;
    std::optional<std::string> plugin_id;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::milliseconds timeout{15000};
    std::string command;
    bool response_attachment_sent = false;
    std::function<void()> stop_progress_interval;
};

/// Result from checking async hook responses
struct AsyncHookResponse {
    std::string process_id;
    cc::utils::json::JsonVal response; // Parsed JSON response
    std::string hook_name;
    std::string hook_event;
    std::optional<std::string> tool_name;
    std::optional<std::string> plugin_id;
    std::string stdout_output;
    std::string stderr_output;
    std::optional<int> exit_code;
};

/// Global registry for pending async hooks
class HookRegistry {
public:
    /// Get the singleton instance
    static HookRegistry& instance() {
        static HookRegistry reg;
        return reg;
    }

    /// Register a new async hook in the pending set
    void register_pending(PendingAsyncHook hook);

    /// Get all pending hooks that haven't had their response sent
    [[nodiscard]] std::vector<PendingAsyncHook*> get_pending_hooks();

    /// Check for completed async hook responses
    [[nodiscard]] async::Task<std::vector<AsyncHookResponse>> check_for_responses();

    /// Remove hooks that have been delivered
    void remove_delivered(const std::vector<std::string>& process_ids);

    /// Finalize all pending hooks (cancel or collect results)
    async::Task<void> finalize_all();

    /// Clear all hooks (for testing)
    void clear_all();

    /// Number of currently pending hooks
    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    HookRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PendingAsyncHook> pending_hooks_;
};

// =========================================================================
// hookHelpers - Utilities for hook execution
// =========================================================================

/// Schema for structured hook responses (ok/reason)
struct HookResponseOutput {
    bool ok = false;
    std::optional<std::string> reason;
};

/// Add arguments to a hook prompt (replaces $ARGUMENTS placeholder)
[[nodiscard]] std::string add_arguments_to_prompt(
    std::string_view prompt,
    std::string_view json_input);

/// Validate a hook response JSON against the expected schema
[[nodiscard]] std::expected<HookResponseOutput, std::string> parse_hook_response(
    std::string_view json_text);

} // namespace cc::utils::hooks_registry
