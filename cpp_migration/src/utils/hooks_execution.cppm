// Hooks Execution Module
// Merges: execAgentHook, execHttpHook, execPromptHook, postSamplingHooks, apiQueryHookHelper
// Provides the core hook execution runners for all hook types
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.hooks_execution;

import cc.utils.json;
import cc.utils.async;
import cc.utils.hooks_registry;

export namespace cc::utils::hooks_execution {

using namespace cc::utils::hooks_registry;
using cc::utils::async::Task;
using cc::utils::json::JsonVal;

// =========================================================================
// Hook Result (unified outcome from any hook execution)
// =========================================================================

/// Outcome of a hook execution
enum class HookResultOutcome {
    Success,           // Hook completed successfully
    Blocking,          // Hook is blocking (exit code 2)
    NonBlockingError,  // Hook failed non-fatally
    Cancelled,         // Hook was cancelled/timed out
};

/// Error info when a hook is blocking
struct BlockingError {
    std::string blocking_error;
    std::string command;
};

/// Attachment message type for hook results
struct HookAttachmentMessage {
    std::string type;       // "hook_success", "hook_non_blocking_error", etc.
    std::string hook_name;
    std::string tool_use_id;
    std::string hook_event;
    std::string content;
    std::string stdout_output;
    std::string stderr_output;
    std::optional<int> exit_code;
};

/// Unified result from executing any hook type
struct HookResult {
    HookCommand hook;
    HookResultOutcome outcome;
    std::optional<BlockingError> blocking_error;
    std::optional<HookAttachmentMessage> message;
};

// =========================================================================
// Abort Signal (cooperative cancellation)
// =========================================================================

/// Lightweight abort signal for cooperative cancellation
class AbortSignal {
public:
    [[nodiscard]] bool is_aborted() const noexcept { return aborted_; }
    void abort() noexcept { aborted_ = true; }

private:
    std::atomic<bool> aborted_{false};
};

// =========================================================================
// executeHook - Universal hook dispatcher
// =========================================================================

/// Context provided to hook execution
struct HookExecutionContext {
    std::string hook_name;
    HookEventType hook_event;
    std::string json_input;         // JSON payload passed to hook
    std::shared_ptr<AbortSignal> signal;
    std::optional<std::string> tool_use_id;
    std::optional<std::string> agent_name;
    std::chrono::milliseconds default_timeout{60000};
};

/// Execute a hook command with the given context
/// Dispatches to the appropriate runner based on hook command type
[[nodiscard]] Task<HookResult> execute_hook(
    const HookCommand& command,
    const HookExecutionContext& context);

// =========================================================================
// AgentHookRunner - Multi-turn LLM agent hook execution
// =========================================================================

/// Configuration for agent hook execution
struct AgentHookRunnerConfig {
    std::string prompt;
    std::optional<std::string> model;
    std::chrono::milliseconds timeout{60000};
    std::size_t max_agent_turns = 50;
    std::string transcript_path;
};

/// Runner for agent-type hooks (multi-turn LLM query)
class AgentHookRunner {
public:
    explicit AgentHookRunner(AgentHookRunnerConfig config);

    /// Execute the agent hook with multi-turn conversation
    [[nodiscard]] Task<HookResult> execute(
        std::string_view hook_name,
        HookEventType hook_event,
        std::string_view json_input,
        std::shared_ptr<AbortSignal> signal,
        std::optional<std::string_view> tool_use_id = std::nullopt);

    /// Get the number of turns executed
    [[nodiscard]] std::size_t turn_count() const noexcept { return turn_count_; }

private:
    AgentHookRunnerConfig config_;
    std::size_t turn_count_ = 0;
};

// =========================================================================
// HttpHookRunner - HTTP webhook execution
// =========================================================================

/// Configuration for HTTP hook execution
struct HttpHookRunnerConfig {
    std::string url;
    std::chrono::milliseconds timeout{600000}; // 10 minutes
    std::optional<std::string> condition;
};

/// SSRF guard policy for HTTP hooks
struct HttpHookPolicy {
    std::optional<std::vector<std::string>> allowed_urls;
};

/// Runner for HTTP-type hooks (webhook calls)
class HttpHookRunner {
public:
    explicit HttpHookRunner(HttpHookRunnerConfig config);

    /// Execute the HTTP hook by posting JSON input to the configured URL
    [[nodiscard]] Task<HookResult> execute(
        std::string_view hook_name,
        HookEventType hook_event,
        std::string_view json_input,
        std::shared_ptr<AbortSignal> signal,
        std::optional<std::string_view> tool_use_id = std::nullopt);

    /// Set the HTTP hook policy (URL allowlist)
    void set_policy(HttpHookPolicy policy);

private:
    HttpHookRunnerConfig config_;
    HttpHookPolicy policy_;
};

// =========================================================================
// PromptHookRunner - Single-turn LLM prompt hook execution
// =========================================================================

/// Configuration for prompt hook execution
struct PromptHookRunnerConfig {
    std::string prompt;
    std::optional<std::string> model;
    std::chrono::milliseconds timeout{60000};
};

/// Runner for prompt-type hooks (single LLM call)
class PromptHookRunner {
public:
    explicit PromptHookRunner(PromptHookRunnerConfig config);

    /// Execute the prompt hook via a single model query
    [[nodiscard]] Task<HookResult> execute(
        std::string_view hook_name,
        HookEventType hook_event,
        std::string_view json_input,
        std::shared_ptr<AbortSignal> signal,
        std::optional<std::string_view> tool_use_id = std::nullopt);

private:
    PromptHookRunnerConfig config_;
};

// =========================================================================
// CommandHookRunner - Shell command hook execution
// =========================================================================

/// Configuration for shell command hook execution
struct CommandHookRunnerConfig {
    std::string command;
    std::string shell = "bash";
    std::chrono::milliseconds timeout{120000}; // 2 minutes
    std::optional<std::string> condition;
};

/// Result of running a shell command hook
struct CommandRunResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code = 0;
    bool timed_out = false;
};

/// Runner for command-type hooks (shell execution)
class CommandHookRunner {
public:
    explicit CommandHookRunner(CommandHookRunnerConfig config);

    /// Execute the command hook in a shell subprocess
    [[nodiscard]] Task<HookResult> execute(
        std::string_view hook_name,
        HookEventType hook_event,
        std::string_view json_input,
        std::shared_ptr<AbortSignal> signal,
        std::optional<std::string_view> tool_use_id = std::nullopt);

    /// Execute and return raw command result (no HookResult wrapping)
    [[nodiscard]] Task<CommandRunResult> run_raw(
        std::string_view json_input,
        std::shared_ptr<AbortSignal> signal);

private:
    CommandHookRunnerConfig config_;
};

// =========================================================================
// Post-Sampling Hooks (postSamplingHooks.ts)
// =========================================================================

/// Run all Stop hooks for the current event, collecting results
[[nodiscard]] Task<std::vector<HookResult>> execute_stop_hooks(
    const std::vector<IndividualHookConfig>& hooks,
    std::string_view json_input,
    std::shared_ptr<AbortSignal> signal);

/// Run all post-tool-use hooks
[[nodiscard]] Task<std::vector<HookResult>> execute_post_tool_hooks(
    const std::vector<IndividualHookConfig>& hooks,
    std::string_view json_input,
    std::shared_ptr<AbortSignal> signal);

// =========================================================================
// API Query Hook Helper (apiQueryHookHelper.ts)
// =========================================================================

/// Helper to execute hooks around API queries
struct ApiQueryHookContext {
    HookEventType event;
    std::string json_input;
    std::vector<IndividualHookConfig> hooks;
    std::shared_ptr<AbortSignal> signal;
};

/// Run all matching hooks for an API query event and return blocking errors if any
[[nodiscard]] Task<std::optional<BlockingError>> run_api_query_hooks(
    const ApiQueryHookContext& context);

// =========================================================================
// Hook condition evaluation
// =========================================================================

/// Check if a hook's "if" condition matches the current context
[[nodiscard]] bool evaluate_hook_condition(
    std::string_view condition,
    std::string_view tool_name,
    std::string_view input_json);

/// Extract the tool/event name from hook input JSON
[[nodiscard]] std::optional<std::string> extract_matcher_field(
    std::string_view json_input,
    std::string_view field_name);

/// Filter hooks by matcher pattern against a given value
[[nodiscard]] std::vector<IndividualHookConfig> filter_hooks_by_matcher(
    const std::vector<IndividualHookConfig>& hooks,
    std::string_view matcher_value);

} // namespace cc::utils::hooks_execution
