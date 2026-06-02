/// @file command_lifecycle.cppm
/// @brief Command execution lifecycle (init->run->complete), prompt submission handling
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <memory>
#include <cstdint>
#include <string_view>
#include <chrono>
#include <unordered_map>

export module cc.utils.command_lifecycle;

import cc.utils.abort_controller;

export namespace cc::utils::command_lifecycle {

using cc::utils::abort_controller::AbortController;
using cc::utils::abort_controller::CancellationToken;

// ---------------------------------------------------------------------------
// Command lifecycle state machine
// ---------------------------------------------------------------------------

/// State of a command execution
enum class CommandLifecycleState : std::uint8_t {
    Started,
    Completed,
};

/// Listener callback for command lifecycle transitions
using CommandLifecycleListener = std::function<void(
    std::string_view uuid,
    CommandLifecycleState state)>;

/// Set the global command lifecycle listener (null to remove)
void set_command_lifecycle_listener(CommandLifecycleListener listener);

/// Notify lifecycle state change for a command
void notify_command_lifecycle(
    std::string_view uuid,
    CommandLifecycleState state);

// ---------------------------------------------------------------------------
// Query source identification
// ---------------------------------------------------------------------------

/// Source of a query/prompt submission
enum class QuerySource : std::uint8_t {
    User,
    Command,
    Resume,
    AutoMode,
    Queue,
    Hook,
};

// ---------------------------------------------------------------------------
// Prompt input mode
// ---------------------------------------------------------------------------

/// Mode of the prompt input
enum class PromptInputMode : std::uint8_t {
    Normal,
    Bash,
    Plan,
};

// ---------------------------------------------------------------------------
// Queued command representation
// ---------------------------------------------------------------------------

/// A command queued for sequential execution
struct QueuedCommand {
    std::string input;
    PromptInputMode mode{PromptInputMode::Normal};
    QuerySource source{QuerySource::Queue};
};

// ---------------------------------------------------------------------------
// Prompt submission handling
// ---------------------------------------------------------------------------

/// Parameters for handling prompt submission
struct HandlePromptSubmitParams {
    std::string input;
    PromptInputMode mode{PromptInputMode::Normal};
    std::vector<QueuedCommand> queued_commands;
    std::string main_loop_model;
    QuerySource query_source{QuerySource::User};
    bool is_external_loading{false};
};

/// Result of processing user input
struct ProcessedInput {
    std::string normalized_input;
    std::optional<std::string> command_name;
    bool should_query{true};
    std::vector<std::string> additional_allowed_tools;
};

/// Process raw user input into a normalized form for execution
[[nodiscard]] auto process_user_input(
    std::string_view raw_input,
    PromptInputMode mode)
    -> std::expected<ProcessedInput, std::string>;

// ---------------------------------------------------------------------------
// Execution context
// ---------------------------------------------------------------------------

/// Context for a tool use execution within a command lifecycle
struct ToolUseContext {
    std::string session_id;
    AbortController* abort_controller{nullptr};
    std::string main_loop_model;
    std::vector<std::string> messages;
};

/// Parameters for executing queued input
struct ExecuteQueuedInputParams {
    std::vector<QueuedCommand> queued_commands;
    std::string main_loop_model;
    std::function<void(std::string_view)> on_input_change;
    std::function<void()> reset_history;
};

/// Execute the next queued command
[[nodiscard]] auto execute_queued_input(const ExecuteQueuedInputParams& params)
    -> std::expected<void, std::string>;

// ---------------------------------------------------------------------------
// Command execution orchestration
// ---------------------------------------------------------------------------

/// Full lifecycle execution: init -> run -> complete
struct CommandExecution {
    std::string uuid;
    CommandLifecycleState state{CommandLifecycleState::Started};
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::chrono::steady_clock::time_point> completed_at;
};

/// Start a new command execution lifecycle
[[nodiscard]] auto start_command_execution()
    -> std::expected<CommandExecution, std::string>;

/// Complete a command execution lifecycle
[[nodiscard]] auto complete_command_execution(CommandExecution& execution)
    -> std::expected<void, std::string>;

} // namespace cc::utils::command_lifecycle
