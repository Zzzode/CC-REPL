/// @file query_engine.cppm
/// @brief Query engine module - the core LLM interaction loop.
/// Manages conversation state, streaming API calls, tool execution loops,
/// retry logic, and token budget management using C++23 coroutines.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <algorithm>
#include <concepts>
#include <limits>
#include <iterator>
#include <ranges>
#include <numeric>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>
#include <random>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <variant>
#include <utility>
#include <httplib.h>

export module cc.query.query_engine;

import cc.types.types;
import cc.tools.tool;
import cc.config.config;
import cc.utils.error;
import cc.utils.json;
import cc.session.storage;
import cc.memdir.paths;
import cc.utils.tool_helpers;
import cc.utils.env_utils;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;
import cc.utils.hooks_registry;
import cc.utils.hooks_execution;
import cc.tools.agent_runtime;
import cc.services.compact.api_microcompact;
import cc.utils.bash_execution;

export namespace cc::core {

// ============================================================
// SSE (Server-Sent Events) stream decoding
// ============================================================

/// A single decoded SSE event: the event type (from the last `event:` line
/// seen) and the concatenated `data:` payload. Emitted by SseEventDecoder.
struct SseEvent {
    std::string type;
    std::string data;
};

/// Pure, stateful decoder for an SSE event stream. Feed it raw byte chunks
/// (which may split events at arbitrary boundaries) and it yields the events
/// that became complete (a blank line terminates an event).
///
/// Extracted from QueryEngine::stream_single_api_call's inline parser so the
/// framing logic (partial chunks, `event:`/`data:` line handling, multi-line
/// data, event-type persistence) is unit-testable without a live HTTP server.
class SseEventDecoder {
public:
    /// Append a raw chunk from the stream and return any events that became
    /// complete. A block with no `data:` line yields no event. The event type
    /// persists across blocks until a new `event:` line is seen (SSE semantics).
    [[nodiscard]] std::vector<SseEvent> feed(std::string_view chunk) {
        std::vector<SseEvent> out;
        buffer_.append(chunk.data(), chunk.size());
        while (true) {
            const auto double_nl = buffer_.find("\n\n");
            if (double_nl == std::string::npos) break;

            std::string event_block = buffer_.substr(0, double_nl);
            buffer_.erase(0, double_nl + 2);

            std::string event_data;
            std::size_t pos = 0;
            while (pos < event_block.size()) {
                const auto nl = event_block.find('\n', pos);
                std::string line;
                if (nl == std::string::npos) {
                    line = event_block.substr(pos);
                    pos = event_block.size();
                } else {
                    line = event_block.substr(pos, nl - pos);
                    pos = nl + 1;
                }
                if (line.starts_with("event: ")) {
                    current_event_type_ = line.substr(7);
                } else if (line.starts_with("data: ")) {
                    if (!event_data.empty()) event_data += '\n';
                    event_data += line.substr(6);
                } else if (line == "data:") {
                    if (!event_data.empty()) event_data += '\n';
                }
            }
            if (!event_data.empty()) {
                out.push_back({current_event_type_, std::move(event_data)});
            }
        }
        return out;
    }

private:
    std::string buffer_;
    std::string current_event_type_;
};

// ============================================================
// Cost and budget tracking types
// ============================================================

/// Cost information for model usage
struct ModelCost {
    double input_tokens_per_million = 3.0;    // Cost per million input tokens
    double output_tokens_per_million = 15.0;  // Cost per million output tokens
};

/// Budget tracking for a session
struct BudgetTracker {
    double max_budget_usd = 10.0;              // Maximum allowed budget
    double current_spend_usd = 0.0;            // Current total spend
    std::unordered_map<std::string, double> model_spend;  // Spend per model
    bool budget_exceeded = false;              // Whether budget was exceeded

    /// Calculate cost for a token usage record
    [[nodiscard]] double calculate_cost(const TokenUsage& usage,
                                         const ModelCost& cost) const noexcept {
        return (static_cast<double>(usage.input_tokens) / 1000000.0 * cost.input_tokens_per_million) +
               (static_cast<double>(usage.output_tokens) / 1000000.0 * cost.output_tokens_per_million);
    }

    /// Track usage and update spend
    void add_usage(const TokenUsage& usage, const std::string& model, const ModelCost& cost) {
        auto cost_this_round = calculate_cost(usage, cost);
        current_spend_usd += cost_this_round;
        model_spend[model] += cost_this_round;
        if (current_spend_usd >= max_budget_usd) {
            budget_exceeded = true;
        }
    }

    /// Check if we can afford more token usage
    [[nodiscard]] bool can_afford(const TokenUsage& estimated_usage,
                                   const ModelCost& cost) const noexcept {
        auto estimated_cost = calculate_cost(estimated_usage, cost);
        return (current_spend_usd + estimated_cost) < max_budget_usd;
    }
};

struct ApiClientConfig {
    std::string base_url{"https://api.anthropic.com"};
    std::string api_key;
    std::string auth_token;  // OAuth/Pro/gateway bearer token; sent as Authorization: Bearer when non-empty (takes precedence over api_key)
    std::string api_version{"2023-06-01"};
    std::chrono::milliseconds timeout{120000};
    int max_retries{3};
    std::chrono::milliseconds base_retry_delay{1000};
};

struct ApiMessagesEndpoint {
    std::string client_base_url;
    std::string path;
};

[[nodiscard]] inline std::expected<ApiMessagesEndpoint, std::string> api_messages_endpoint(
    std::string_view base_url
) {
    const auto scheme_end = base_url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected("API base URL must include http:// or https://");
    }

    const auto authority_start = scheme_end + 3;
    const auto path_start = base_url.find('/', authority_start);
    std::string client_base_url = path_start == std::string_view::npos
        ? std::string(base_url)
        : std::string(base_url.substr(0, path_start));
    std::string path_prefix = path_start == std::string_view::npos
        ? std::string{}
        : std::string(base_url.substr(path_start));

    while (client_base_url.size() > scheme_end + 3 && client_base_url.ends_with('/')) {
        client_base_url.pop_back();
    }
    while (!path_prefix.empty() && path_prefix.ends_with('/')) {
        path_prefix.pop_back();
    }
    if (path_prefix == "/") path_prefix.clear();

    if (client_base_url.size() <= scheme_end + 3) {
        return std::unexpected("API base URL host cannot be empty");
    }

    std::string path = path_prefix;
    path += path.ends_with("/v1") ? "/messages" : "/v1/messages";
    return ApiMessagesEndpoint{
        .client_base_url = std::move(client_base_url),
        .path = std::move(path),
    };
}

/// Permission denial record
struct PermissionDenial {
    std::string tool_name;
    std::string tool_use_id;
    std::string tool_input;
};

// ============================================================
// Configuration types for the query engine
// ============================================================

/// Model parameters for API requests
struct ModelParams {
    std::string model = "claude-sonnet-4-20250514";  // Default model
    std::uint32_t max_tokens = 16384;                 // Max output tokens
    std::optional<double> temperature;                // Sampling temperature
    std::optional<double> top_p;                      // Nucleus sampling
    std::optional<std::uint32_t> top_k;              // Top-k sampling
    bool extended_thinking = false;                    // Enable thinking blocks
    std::optional<std::uint32_t> thinking_budget;     // Max thinking tokens
};

/// Retry policy configuration
struct RetryPolicy {
    std::uint32_t max_retries = 3;                    // Maximum retry attempts
    std::chrono::milliseconds initial_delay{1000};    // First retry delay
    double backoff_multiplier = 2.0;                  // Exponential backoff factor
    std::chrono::milliseconds max_delay{30000};       // Maximum delay cap
    bool retry_on_overload = true;                    // Retry 529 overloaded errors
    bool retry_on_rate_limit = true;                  // Retry 429 rate limit errors
};

/// Context window management settings
struct ContextWindowConfig {
    std::uint32_t max_context_tokens = 200000;    // Model's context window size
    double warning_threshold = 0.8;                // Warn when usage exceeds this ratio
    double compaction_threshold = 0.9;             // Auto-compact when exceeding this
    bool auto_compact = true;                      // Enable automatic compaction
};

/// Thinking configuration
struct ThinkingConfig {
    enum class Mode { Disabled, Adaptive, Forced };
    Mode mode = Mode::Adaptive;
    std::optional<std::uint32_t> budget_tokens;
};

/// Full query engine configuration
struct QueryEngineConfig {
    ModelParams model_params;
    RetryPolicy retry_policy;
    ContextWindowConfig context_window;
    ThinkingConfig thinking_config;
    std::string api_key;                            // Anthropic API key (x-api-key)
    std::string auth_token;                         // OAuth/gateway bearer token (Authorization: Bearer); takes precedence over api_key when set
    std::optional<std::string> base_url;            // Custom API base URL
    std::optional<std::string> custom_system_prompt;// Custom system prompt
    std::optional<std::string> append_system_prompt;// Append to default system prompt
    std::optional<std::string> cwd;                 // Working directory for operations
    std::optional<double> max_budget_usd;           // Max budget in USD
    std::optional<std::uint32_t> max_turns;         // Max conversation turns
    struct TaskBudget {
        std::uint32_t total{0};
        std::optional<std::uint32_t> remaining;
    };
    std::optional<TaskBudget> task_budget;           // API-side output_config.task_budget
    struct ResponseSchema {
        std::string name;                            // schema name (json_schema.name)
        std::string schema_json;                     // JSON schema payload (string)
    };
    std::optional<ResponseSchema> response_schema;   // API-side output_config.format.json_schema
    std::vector<ToolDefinition> tools;              // Available tools for the model
    std::vector<std::string> agent_definitions;     // Agent definitions
    std::vector<std::string> fallback_models;       // Fallback models for capacity errors
};

// ============================================================
// Streaming callback interface
// ============================================================

/// Callback invoked for each stream event during response generation
using StreamCallback = std::function<void(const StreamEvent&)>;

/// Query options for a single request
struct QueryOptions {
    std::optional<StreamCallback> on_event;        // Per-event callback
    bool include_thinking = true;                   // Include thinking in response
    std::optional<std::uint32_t> max_tool_rounds;  // Limit tool-call iterations
    std::vector<std::string> enabled_tools;        // Subset of tools to enable
    std::optional<std::string> prompt_uuid;        // UUID for this prompt
    bool is_meta = false;                           // Whether this is a meta prompt
    // AT-02: materialized @-mention file attachments (TextBlock/ImageBlock/...).
    // stream_query appends these to the user message content after the text
    // block so the model actually sees file contents — faithful to TS
    // utils/attachments.ts (the C++ port previously passed "@path" literally,
    // making @ a no-op for the model).
    std::vector<ContentBlock> attachments;
};

// ============================================================
// Query result types
// ============================================================

/// Complete response from a query (after all tool loops resolve)
struct QueryResponse {
    AssistantMessage message;       // Final assistant message
    TokenUsage total_usage;         // Cumulative token usage across all rounds
    std::uint32_t tool_rounds = 0;  // Number of tool-call iterations
    std::chrono::milliseconds elapsed{0};  // Total wall-clock time
    bool budget_exceeded = false;   // Whether budget was hit
    bool success = true;            // Whether query completed successfully
    std::vector<std::string> errors;// Any errors encountered
};

// ============================================================
// User context and system context builders
// ============================================================

/// User context for system prompt building
struct UserContext {
    std::string cwd;
    std::string platform;
    std::string username;
    std::vector<std::string> additional_contexts;
};

/// System context for system prompt building
struct SystemContext {
    std::vector<std::string> tool_descriptions;
    std::string model_info;
    std::string permission_level;
};

/// System prompt builder
class SystemPromptBuilder {
public:
    /// Build a complete system prompt from components
    [[nodiscard]] static std::string build(
        std::optional<std::string_view> custom_prompt,
        std::optional<std::string_view> append_prompt,
        const UserContext& user_ctx,
        const SystemContext& system_ctx) {
        std::string prompt;

        // Base prompt from custom or default
        if (custom_prompt) {
            prompt = std::string(*custom_prompt);
        } else {
            prompt = build_default_system_prompt();
        }

        // Append additional instructions
        if (append_prompt) {
            prompt += "\n\n";
            prompt += *append_prompt;
        }

        // Add context information
        prompt += "\n\n<context>";
        prompt += std::format("\n<cwd>{}</cwd>", user_ctx.cwd);
        prompt += std::format("\n<platform>{}</platform>", user_ctx.platform);

        // Add tool descriptions
        if (!system_ctx.tool_descriptions.empty()) {
            prompt += "\n<tools>";
            for (const auto& tool : system_ctx.tool_descriptions) {
                prompt += std::format("\n  - {}", tool);
            }
            prompt += "\n</tools>";
        }

        for (const auto& ctx : user_ctx.additional_contexts) {
            prompt += "\n";
            prompt += ctx;
        }
        prompt += "\n</context>";

        return prompt;
    }

private:
    /// Build default system prompt
    [[nodiscard]] static std::string build_default_system_prompt() {
        return R"(You are Claude, a helpful assistant working with code and files.
You have access to various tools to read, write, edit, and search files, execute commands, and more.
Always use the appropriate tools to accomplish your tasks rather than trying to do everything manually.
When you write or edit files, always make complete, functional changes.)";
    }
};

// ============================================================
// Query Engine - main LLM interaction engine
// ============================================================

/// Core engine orchestrating LLM API interactions, tool execution,
/// and conversation state management.
class QueryEngine {
public:
    /// Construct engine with configuration and tool registry
    explicit QueryEngine(QueryEngineConfig config, ToolRegistry& registry)
        : config_(std::move(config)), tool_registry_(&registry),
          session_start_(std::chrono::system_clock::now()) {
        // Initialize session ID
        session_id_.value = generate_session_id();

        // Set up budget
        if (config_.max_budget_usd) {
            budget_tracker_.max_budget_usd = *config_.max_budget_usd;
        }

        // Initialize API client config
        setup_api_client();

        // Initialize conversation with system prompt if available
        build_and_add_system_prompt();
    }

    // Non-copyable, movable
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    QueryEngine(QueryEngine&&) = delete;
    QueryEngine& operator=(QueryEngine&&) = delete;

    // ============================================================
    // Primary query interfaces
    // ============================================================

    /// Send a query and return the complete response after all tool loops.
    /// This is the blocking, complete-response interface.
    [[nodiscard]] Result<QueryResponse> query(
        std::string_view user_message,
        const QueryOptions& options = {}) {
        auto start = std::chrono::steady_clock::now();

        // Reset abort flag for new query
        aborted_.store(false);

        // Check budget first
        if (budget_tracker_.budget_exceeded) {
            return std::unexpected(Error::make(
                ErrorCode::ContextWindowExceeded,
                std::format("Budget exceeded: ${:.4f} of ${:.4f}",
                    budget_tracker_.current_spend_usd,
                    budget_tracker_.max_budget_usd)));
        }

        // Construct user message and append to history
        auto msg = make_user_message(user_message, options.prompt_uuid);
        append_message(Message{std::move(msg)});

        // Execute the tool-call loop
        auto result = execute_tool_loop(options);
        if (!result) return std::unexpected(result.error());

        // Append the final assistant message to conversation for persistence
        append_message(Message{result->message});

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        auto& [response_msg, usage, rounds] = *result;
        return QueryResponse{
            std::move(response_msg),
            usage,
            rounds,
            elapsed,
            budget_tracker_.budget_exceeded,
            true,
            {}
        };
    }

    /// Stream a query with real-time event callbacks
    void stream_query(
        std::string_view user_message,
        const QueryOptions& options = {}) {
        if (budget_tracker_.budget_exceeded) {
            if (options.on_event) {
                StreamError err{"budget_exceeded",
                    std::format("Budget exceeded: ${:.4f} of ${:.4f}",
                        budget_tracker_.current_spend_usd,
                        budget_tracker_.max_budget_usd)};
                (*options.on_event)(err);
            }
            return;
        }

        // Reset abort flag for new query
        aborted_.store(false);
        auto query_start_time = std::chrono::steady_clock::now();

        // Emit query start hook
        if (lifecycle_hooks_) {
            lifecycle_hooks_->emit_query_start(cc::hooks::QueryStartEvent{
                .query_text = std::string(user_message),
                .model = config_.model_params.model,
                .timestamp = std::chrono::system_clock::now()
            });
        }

        auto msg = make_user_message(user_message, options.prompt_uuid);
        // AT-02: append materialized @-mention attachments so the model sees
        // the referenced file contents (not the literal "@path" string).
        for (const auto& block : options.attachments) {
            msg.content.push_back(block);
        }
        append_message(Message{std::move(msg)});

        std::uint32_t round = 0;
        std::uint32_t continuation_count = 0;
        std::uint32_t max_tokens_retries = 0;
        const std::uint32_t max_rounds = options.max_tool_rounds.value_or(20);

        while (round < max_rounds && !should_abort() && !budget_tracker_.budget_exceeded) {
            // Check stop hooks before each iteration
            if (lifecycle_hooks_) {
                if (auto stop_reason = lifecycle_hooks_->check_stop_hooks()) {
                    if (options.on_event) {
                        StreamError err{"hook_stop",
                            std::format("Query stopped by hook: {}", *stop_reason)};
                        (*options.on_event)(err);
                    }
                    break;
                }
            }

            append_pending_native_agent_notifications();
            // Stream a single API call
            auto stream_call = stream_single_api_call(options);
            if (stream_call.failed) {
                break;
            }
            auto& assistant_msg = stream_call.message;
            auto& round_usage = stream_call.usage;
            const bool has_tool_use = stream_call.has_tool_use;

            budget_tracker_.add_usage(round_usage, config_.model_params.model, model_cost_);
            cumulative_usage_ += round_usage;

            append_message(Message{assistant_msg});

            // P1-3: Max output tokens recovery (streaming path)
            if (assistant_msg.stop_reason == "max_tokens" && max_tokens_retries < 3) {
                config_.model_params.max_tokens = std::max(config_.model_params.max_tokens, std::uint32_t{64000});
                auto resume = make_user_message(
                    "Your previous response was cut off at the token limit. "
                    "Please continue from where you left off.");
                append_message(Message{std::move(resume)});
                ++max_tokens_retries;
                ++round;
                continue;
            }

            // If no tool use requested, check for budget continuation
            if (!has_tool_use) {
                // P1-10: Token budget continuation (auto-continue when under budget)
                if (token_budget_ > 0 && continuation_count < 5) {
                    auto used = cumulative_usage_.output_tokens;
                    if (used < static_cast<std::uint32_t>(token_budget_ * 0.9)) {
                        auto pct = (used * 100) / token_budget_;
                        auto nudge = std::format(
                            "You've used {}% of the output budget ({}/{} tokens). "
                            "Keep working — do not summarize or stop early.",
                            pct, used, token_budget_);
                        auto cont_msg = make_user_message(nudge);
                        append_message(Message{std::move(cont_msg)});
                        ++continuation_count;
                        ++round;
                        continue;
                    }
                }
                break;  // Done — no more tool calls, budget satisfied
            }

            // Execute tools and feed results back
            auto tool_results = execute_pending_tools(assistant_msg, options);
            for (auto& tr : tool_results) {
                append_message(Message{std::move(tr)});
            }

            ++round;
        }

        if (budget_tracker_.budget_exceeded && options.on_event) {
            (*options.on_event)(StreamError{"budget_exceeded", "Budget limit reached"});
        }

        // Emit query end hook
        if (lifecycle_hooks_) {
            auto query_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - query_start_time);
            lifecycle_hooks_->emit_query_end(cc::hooks::QueryEndEvent{
                .success = !budget_tracker_.budget_exceeded,
                .rounds = round,
                .tools_executed = round,  // Approximate
                .duration = query_duration,
                .timestamp = std::chrono::system_clock::now()
            });
        }
    }

    /// Abort any in-flight query (thread-safe)
    void abort() noexcept { aborted_.store(true); }

    /// Reset the abort flag for new queries
    void reset_abort() noexcept { aborted_.store(false); }

    /// Set an external cancellation predicate for host-driven query control.
    void set_external_abort_callback(std::function<bool()> callback) {
        external_abort_callback_ = std::move(callback);
    }

    /// Set the permission hook for tool execution policy
    void set_permission_hook(cc::hooks::ToolPermissionHook* hook) noexcept {
        permission_hook_ = hook;
    }

    /// Set the lifecycle hook registry for event notifications
    void set_lifecycle_hooks(cc::hooks::LifecycleHookRegistry* hooks) noexcept {
        lifecycle_hooks_ = hooks;
    }

    /// Configure the user-configured hook path. Mirrors the TS wiring where
    /// src/services/tools/toolExecution.ts runs PreToolUse/PostToolUse hooks
    /// from src/utils/hooks.ts (the execution engine) alongside the in-process
    /// lifecycle event bus. `ctx_template` supplies stable context vars
    /// (session/conversation ids); per-call context (tool name, payload) is
    /// merged at dispatch time in execute_single_tool.
    void set_user_hooks(
        std::vector<cc::utils::hooks_registry::IndividualHookConfig> registry,
        cc::utils::hooks_execution::HookExecutionContext ctx_template) {
        user_hooks_ = std::move(registry);
        user_hooks_ctx_template_ = std::move(ctx_template);
        user_hooks_configured_ = true;
    }

    // ============================================================
    // Conversation management
    // ============================================================

    /// Get current token usage for the session
    [[nodiscard]] TokenUsage get_usage() const noexcept { return cumulative_usage_; }

    /// Get the configured model context window size.
    [[nodiscard]] std::uint32_t max_context_tokens() const noexcept {
        return config_.context_window.max_context_tokens;
    }

    /// Get estimated context window utilization ratio [0.0, 1.0]
    [[nodiscard]] double context_utilization() const noexcept {
        auto estimated = estimate_conversation_tokens();
        return static_cast<double>(estimated) / config_.context_window.max_context_tokens;
    }

    /// Get the full conversation history (thread-safe copy)
    [[nodiscard]] std::vector<Message> get_conversation() const {
        std::lock_guard lock(conversation_mutex_);
        return conversation_;
    }

    /// Restore conversation history for resumed sessions and rebuild content replacement state.
    void restore_conversation(std::vector<Message> messages) {
        std::lock_guard lock(conversation_mutex_);
        conversation_ = std::move(messages);
        restore_task_budget_remaining_from_compact_boundaries_locked();
        rebuild_content_replacement_state_locked();
    }

    /// Clear conversation history (start fresh within same session)
    void clear_conversation() {
        std::lock_guard lock(conversation_mutex_);
        conversation_.clear();
        build_and_add_system_prompt();
    }

    /// Compact conversation history to fit within context window.
    /// Preserves system prompt and recent messages, summarizes middle.
    [[nodiscard]] cc::utils::VoidResult compact_conversation(std::string_view trigger = "manual") {
        std::lock_guard lock(conversation_mutex_);
        if (conversation_.size() <= 4) {
            return {};  // Nothing to compact
        }

        // Keep first (system) and last N messages, drop middle
        constexpr std::size_t keep_recent = 6;
        if (conversation_.size() <= keep_recent + 1) return {};

        const auto pre_tokens = estimate_conversation_tokens_locked();
        update_task_budget_remaining_after_compact(pre_tokens);
        auto recent_start = conversation_.end() - static_cast<std::ptrdiff_t>(keep_recent);
        auto summary_text = build_compaction_summary(
            conversation_.begin() + 1,
            recent_start);

        std::vector<Message> retained_recent;
        retained_recent.reserve(keep_recent);
        for (auto it = recent_start; it != conversation_.end(); ++it) {
            if (is_compact_boundary_message(*it)) continue;
            retained_recent.push_back(*it);
        }

        const auto summary_id = generate_id();
        CompactMetadata metadata{
            .trigger = std::string(trigger),
            .pre_tokens = pre_tokens,
            .preserved_segment = compact_preserved_segment(retained_recent.begin(), retained_recent.end(), summary_id),
        };

        // Replace middle section with a compact boundary and summary marker.
        std::vector<Message> compacted;
        compacted.push_back(conversation_.front());  // System prompt

        SystemMessage boundary{};
        boundary.id.value = generate_id();
        boundary.timestamp = std::chrono::system_clock::now();
        boundary.subtype = "compact_boundary";
        boundary.compact_metadata = std::move(metadata);
        boundary.content.push_back(TextBlock{std::format(
            "Conversation compacted by {} compact.",
            trigger.empty() ? std::string_view{"manual"} : trigger)});
        compacted.push_back(Message{std::move(boundary)});

        UserMessage marker{};
        marker.id.value = summary_id;
        marker.timestamp = std::chrono::system_clock::now();
        marker.content.push_back(TextBlock{std::move(summary_text)});
        compacted.push_back(Message{std::move(marker)});

        // Keep recent messages, dropping stale compact boundaries from prior compaction chains.
        compacted.insert(compacted.end(), retained_recent.begin(), retained_recent.end());

        conversation_ = std::move(compacted);
        return {};
    }

    void append_message_for_testing(Message msg) {
        append_message(std::move(msg));
    }

    /// Update model parameters at runtime
    void set_model_params(ModelParams params) { config_.model_params = std::move(params); }

    /// Get current model parameters
    [[nodiscard]] const ModelParams& model_params() const noexcept { return config_.model_params; }

    /// Get the effective working directory used by the engine.
    [[nodiscard]] std::string working_directory() const {
        return config_.cwd.value_or(std::filesystem::current_path().string());
    }

    /// Get session ID
    [[nodiscard]] const SessionId& session_id() const noexcept { return session_id_; }

    /// Skills invoked via the skill tool during this session (in-loop dispatch
    /// tracking). Populated by execute_single_tool on each successful skill call.
    [[nodiscard]] std::vector<std::string> discovered_skills() const {
        std::lock_guard lock(state_mutex_);
        return {discovered_skills_.begin(), discovered_skills_.end()};
    }

    /// Testing seam: invoke a single tool-use block through the same path as
    /// the live tool loop (so in-loop tracking like discovered_skills_ fires).
    [[nodiscard]] ToolResultMessage execute_single_tool_for_testing(
        const ToolUseBlock& tool_use) {
        QueryOptions options;
        return execute_single_tool(tool_use, options);
    }

    /// Get permission denials
    [[nodiscard]] std::vector<PermissionDenial> get_permission_denials() const {
        std::lock_guard lock(state_mutex_);
        return permission_denials_;
    }

    /// Get current budget status
    [[nodiscard]] const BudgetTracker& budget_tracker() const noexcept { return budget_tracker_; }

private:
    // ============================================================
    // Internal implementation
    // ============================================================

    /// Setup API client configuration
    void setup_api_client() {
        api_config_.base_url = config_.base_url.value_or("https://api.anthropic.com");
        api_config_.api_key = config_.api_key;
        api_config_.auth_token = config_.auth_token;
        api_config_.api_version = "2023-06-01";
        api_config_.timeout = std::chrono::milliseconds{120000};
        api_config_.max_retries = static_cast<int>(config_.retry_policy.max_retries);
        api_config_.base_retry_delay = config_.retry_policy.initial_delay;
    }

    /// Build and add system prompt to conversation
    void build_and_add_system_prompt() {
        auto cwd = config_.cwd.value_or(std::filesystem::current_path().string());

        UserContext user_ctx{
            cwd,
            "unknown",
            "user",
            {}
        };

        // P1-12: Inject git context (branch + last commit)
        populate_git_context(user_ctx, cwd);

        // P1-13: Load CLAUDE.md memory file
        auto claude_md = load_claude_md(std::filesystem::path(cwd));
        if (!claude_md.empty()) {
            user_ctx.additional_contexts.push_back(
                std::format("<context name=\"CLAUDE.md\">\n{}\n</context>", claude_md));
        }

        // P1-13b: Load user-level memory (~/.claude/CLAUDE.md) via the memdir
        // module so global user preferences are injected alongside project
        // memory. Tree/ancestor CLAUDE.md is already covered by load_claude_md.
        auto user_mem = cc::memdir::get_user_memory_path();
        if (std::filesystem::exists(user_mem)) {
            std::ifstream um_ifs(user_mem);
            if (um_ifs) {
                std::string um_content((std::istreambuf_iterator<char>(um_ifs)), {});
                if (!um_content.empty()) {
                    loaded_nested_memory_paths_.insert(user_mem.string());
                    user_ctx.additional_contexts.push_back(
                        std::format("<context name=\"UserMemory\">\n{}\n</context>", um_content));
                }
            }
        }

        // P1-1: Add tool descriptions to system context
        SystemContext system_ctx{
            {},
            config_.model_params.model,
            "default"
        };
        for (const auto& tool : config_.tools) {
            system_ctx.tool_descriptions.push_back(tool.name);
        }

        auto prompt = SystemPromptBuilder::build(
            config_.custom_system_prompt,
            config_.append_system_prompt,
            user_ctx,
            system_ctx);

        SystemMessage sys_msg{};
        sys_msg.id.value = generate_id();
        sys_msg.timestamp = std::chrono::system_clock::now();
        sys_msg.content.push_back(TextBlock{std::move(prompt)});
        conversation_.push_back(Message{std::move(sys_msg)});
    }

    /// Populate user context with git branch and last commit info
    static void populate_git_context(UserContext& ctx, const std::string& cwd) {
        auto run_git_cmd = [&](const char* cmd) -> std::string {
            auto full_cmd = std::format("cd \"{}\" && ( {} ) 2>/dev/null", cwd, cmd);
            std::unique_ptr<FILE, decltype(&pclose)> pipe(cc::utils::bash::popen_spawn(full_cmd.c_str()), pclose);
            if (!pipe) return {};
            std::string result;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe.get())) {
                result += buf;
            }
            // Trim trailing newline
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
            return result;
        };

        auto branch = run_git_cmd("git rev-parse --abbrev-ref HEAD");
        auto commit = run_git_cmd("git log --oneline -1");
        auto status_short = run_git_cmd("git status --short | head -20");

        if (!branch.empty()) {
            std::string git_ctx = std::format(
                "<context name=\"git\">\nBranch: {}\nLast commit: {}", branch, commit);
            if (!status_short.empty()) {
                git_ctx += std::format("\nStatus:\n{}", status_short);
            }
            git_ctx += "\n</context>";
            ctx.additional_contexts.push_back(std::move(git_ctx));
        }
    }

    /// Load CLAUDE.md from CWD or parent directories
    [[nodiscard]] static std::string load_claude_md(const std::filesystem::path& cwd) {
        auto path = cwd;
        for (int depth = 0; depth < 10; ++depth) {
            auto claude_md = path / "CLAUDE.md";
            if (std::filesystem::exists(claude_md)) {
                std::ifstream ifs(claude_md);
                if (ifs) {
                    return std::string(std::istreambuf_iterator<char>(ifs), {});
                }
            }
            auto parent = path.parent_path();
            if (parent == path) break;
            path = parent;
        }
        return {};
    }

    /// Build a UserMessage from raw text input
    [[nodiscard]] UserMessage make_user_message(std::string_view text,
                                                 std::optional<std::string_view> uuid = std::nullopt) const {
        UserMessage msg{};
        if (uuid) {
            msg.id.value = std::string(*uuid);
        } else {
            msg.id.value = generate_id();
        }
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(TextBlock{std::string(text)});
        return msg;
    }

    /// Thread-safe append to conversation history
public:
    /// Enable transcript persistence. After this call, every appended message
    /// is also appended (JSONL) to <sessions_dir>/<session_id>/messages.jsonl,
    /// and session metadata is written so the session is discoverable by
    /// cc::session::list_recent_sessions. Call once at startup.
    void set_session_storage(std::filesystem::path sessions_dir) {
        sessions_dir_ = std::move(sessions_dir);
        cc::session::SessionMetadata meta{
            .session_id = session_id_.str(),
            .model = config_.model_params.model,
            .cwd = std::filesystem::current_path(),
            .created_at = session_start_,
            .last_active = std::chrono::system_clock::now(),
            .message_count = 0,
            .title = std::nullopt,
            .is_archived = false,
        };
        (void)cc::session::save_session_metadata(*sessions_dir_, meta);
    }

    /// Refresh session metadata on disk. Message bodies are appended
    /// incrementally in append_message(), so this only rewrites metadata.json
    /// with the current message count and last-active timestamp.
    void flush_session() {
        if (!sessions_dir_) return;
        std::size_t count = 0;
        {
            std::lock_guard lock(conversation_mutex_);
            for (const auto& m : conversation_) {
                // SystemMessage is not persisted to the transcript (it is
                // rebuilt dynamically each query), so exclude it from the
                // on-disk message_count to match messages.jsonl line count.
                if (!std::holds_alternative<SystemMessage>(m)) ++count;
            }
        }
        cc::session::SessionMetadata meta{
            .session_id = session_id_.str(),
            .model = config_.model_params.model,
            .cwd = std::filesystem::current_path(),
            .created_at = session_start_,
            .last_active = std::chrono::system_clock::now(),
            .message_count = static_cast<int>(count),
            .title = std::nullopt,
            .is_archived = false,
        };
        (void)cc::session::save_session_metadata(*sessions_dir_, meta);
    }

private:
    /// Serialize a single Message to a one-line JSON string for the transcript.
    /// Reuses the API-request serializer; SystemMessage has no on-wire form and
    /// serializes to empty (skipped by append_message).
    [[nodiscard]] std::string message_to_jsonl_(const Message& msg) const {
        cc::utils::json::JsonMutDoc doc;
        auto arr = doc.array();
        append_message_to_json(msg, arr, doc);
        doc.set_root(arr);
        auto s = doc.to_string();
        // arr is a single-element array "[{...}]" -> strip outer brackets.
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    void append_message(Message msg) {
        bool should_compact = false;
        std::string persist_json;
        bool do_persist = false;
        {
            std::lock_guard lock(conversation_mutex_);
            // Serialize for disk persistence BEFORE moving (if enabled).
            if (sessions_dir_) {
                persist_json = message_to_jsonl_(msg);
                do_persist = !persist_json.empty();
            }
            conversation_.push_back(std::move(msg));

            // Check if auto-compact needed (but don't call it while holding lock)
            if (config_.context_window.auto_compact &&
                context_utilization() > config_.context_window.compaction_threshold) {
                should_compact = true;
            }
        }
        // Persist transcript line (outside the conversation lock; ofstream is
        // not part of the engine critical section). SystemMessage serializes
        // empty and is skipped.
        if (do_persist) {
            (void)cc::session::append_message(
                *sessions_dir_, session_id_.str(), persist_json);
        }
        // Lock released — safe to call compact which re-acquires
        if (should_compact) {
            (void)compact_conversation("auto");
        }
    }

    void append_pending_native_agent_notifications() {
        for (auto& notification : cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications()) {
            auto msg = make_user_message(notification);
            append_message(Message{std::move(msg)});
        }
    }

    struct QueryToolResultBudgetCandidate {
        std::size_t message_index = 0;
        std::string tool_use_id;
        std::string tool_name;
        std::size_t size = 0;
    };

    [[nodiscard]] static std::string lowercase_ascii(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
            if (ch >= 'A' && ch <= 'Z') {
                out.push_back(static_cast<char>(ch - 'A' + 'a'));
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
        return out;
    }

    [[nodiscard]] static bool tool_result_already_replaced(std::string_view text) {
        return text.starts_with(cc::utils::PERSISTED_OUTPUT_TAG);
    }

    [[nodiscard]] static bool env_truthy_any(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            if (cc::utils::is_env_truthy(std::getenv(name))) return true;
        }
        return false;
    }

    [[nodiscard]] static std::uint32_t env_uint_or_default(
        std::initializer_list<const char*> names,
        std::uint32_t fallback) {
        for (const char* name : names) {
            const char* raw = std::getenv(name);
            if (raw == nullptr || *raw == '\0') continue;
            char* end = nullptr;
            const auto value = std::strtoul(raw, &end, 10);
            if (end == raw || value == 0) continue;
            return static_cast<std::uint32_t>(
                std::min<unsigned long>(value, std::numeric_limits<std::uint32_t>::max()));
        }
        return fallback;
    }

    [[nodiscard]] static bool time_based_microcompact_enabled() {
        return env_truthy_any({
            "CC_REPL_TIME_BASED_MICROCOMPACT",
            "CLAUDE_CODE_TIME_BASED_MICROCOMPACT",
        });
    }

    [[nodiscard]] static std::uint32_t time_based_microcompact_gap_minutes() {
        return env_uint_or_default({
            "CC_REPL_TIME_BASED_MICROCOMPACT_GAP_MINUTES",
            "CLAUDE_CODE_TIME_BASED_MICROCOMPACT_GAP_MINUTES",
        }, 60);
    }

    [[nodiscard]] static std::uint32_t time_based_microcompact_keep_recent() {
        return std::max<std::uint32_t>(1, env_uint_or_default({
            "CC_REPL_TIME_BASED_MICROCOMPACT_KEEP_RECENT",
            "CLAUDE_CODE_TIME_BASED_MICROCOMPACT_KEEP_RECENT",
        }, 5));
    }

    [[nodiscard]] static bool is_time_based_microcompact_tool(std::string_view tool_name) {
        const auto name = lowercase_ascii(tool_name);
        static const std::unordered_set<std::string> compactable = {
            "bash",
            "powershell",
            "glob",
            "grep",
            "read",
            "webfetch",
            "websearch",
            "edit",
            "write",
        };
        return compactable.contains(name);
    }

    [[nodiscard]] std::unordered_set<std::string> unbounded_tool_result_budget_names() const {
        std::unordered_set<std::string> names;
        if (!tool_registry_) return names;
        for (const auto& definition : tool_registry_->get_visible_definitions()) {
            if (definition.max_result_size_unbounded) {
                names.insert(lowercase_ascii(definition.name));
            }
        }
        return names;
    }

    [[nodiscard]] static std::unordered_map<std::string, std::string> tool_name_by_tool_use_id(
        const std::vector<Message>& messages) {
        std::unordered_map<std::string, std::string> names;
        for (const auto& message : messages) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
                for (const auto& block : assistant->content) {
                    if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                        names[tool_use->id.value] = tool_use->name;
                    }
                }
            } else if (const auto* tool_use_message = std::get_if<ToolUseMessage>(&message)) {
                for (const auto& block : tool_use_message->content) {
                    if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                        names[tool_use->id.value] = tool_use->name;
                    }
                }
            }
        }
        return names;
    }

    [[nodiscard]] static std::optional<std::string> tool_result_plain_text_content(
        const ToolResultMessage& message) {
        std::string text;
        for (const auto& block : message.content) {
            if (const auto* text_block = std::get_if<TextBlock>(&block)) {
                text += text_block->text;
            } else {
                return std::nullopt;
            }
        }
        if (text.empty()) {
            return std::nullopt;
        }
        return text;
    }

    [[nodiscard]] static std::optional<std::string> tool_result_text_for_budget(
        const ToolResultMessage& message) {
        auto text = tool_result_plain_text_content(message);
        if (!text || tool_result_already_replaced(*text)) {
            return std::nullopt;
        }
        return text;
    }

    [[nodiscard]] static std::vector<std::vector<QueryToolResultBudgetCandidate>>
    collect_tool_result_budget_candidates_by_message(
        const std::vector<Message>& messages,
        const std::unordered_map<std::string, std::string>& tool_names) {
        std::vector<std::vector<QueryToolResultBudgetCandidate>> groups;
        std::vector<QueryToolResultBudgetCandidate> current;
        auto flush = [&] {
            if (!current.empty()) groups.push_back(std::exchange(current, {}));
        };

        for (std::size_t message_index = 0; message_index < messages.size(); ++message_index) {
            const auto& message = messages[message_index];
            if (std::holds_alternative<AssistantMessage>(message) ||
                std::holds_alternative<ToolUseMessage>(message)) {
                flush();
                continue;
            }
            const auto* tool_result = std::get_if<ToolResultMessage>(&message);
            if (!tool_result || tool_result->tool_use_id.value.empty()) continue;
            auto text = tool_result_text_for_budget(*tool_result);
            if (!text) continue;
            std::string tool_name;
            if (auto name = tool_names.find(tool_result->tool_use_id.value); name != tool_names.end()) {
                tool_name = name->second;
            }
            current.push_back(QueryToolResultBudgetCandidate{
                .message_index = message_index,
                .tool_use_id = tool_result->tool_use_id.value,
                .tool_name = std::move(tool_name),
                .size = text->size(),
            });
        }
        flush();
        return groups;
    }

    [[nodiscard]] static std::optional<std::chrono::system_clock::time_point>
    last_assistant_timestamp(const std::vector<Message>& messages) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&*it)) {
                return assistant->timestamp;
            }
            if (const auto* tool_use = std::get_if<ToolUseMessage>(&*it)) {
                return tool_use->timestamp;
            }
        }
        return std::nullopt;
    }

    void apply_time_based_microcompact() {
        if (!time_based_microcompact_enabled()) return;

        std::lock_guard lock(conversation_mutex_);
        const auto last_assistant = last_assistant_timestamp(conversation_);
        if (!last_assistant) return;

        const auto now = std::chrono::system_clock::now();
        if (*last_assistant > now) return;
        const auto gap = std::chrono::duration_cast<std::chrono::minutes>(now - *last_assistant);
        if (gap < std::chrono::minutes{time_based_microcompact_gap_minutes()}) return;

        std::vector<std::string> compactable_ids;
        for (const auto& message : conversation_) {
            if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
                for (const auto& block : assistant->content) {
                    const auto* tool_use = std::get_if<ToolUseBlock>(&block);
                    if (tool_use && is_time_based_microcompact_tool(tool_use->name)) {
                        compactable_ids.push_back(tool_use->id.value);
                    }
                }
            } else if (const auto* tool_use_message = std::get_if<ToolUseMessage>(&message)) {
                for (const auto& block : tool_use_message->content) {
                    const auto* tool_use = std::get_if<ToolUseBlock>(&block);
                    if (tool_use && is_time_based_microcompact_tool(tool_use->name)) {
                        compactable_ids.push_back(tool_use->id.value);
                    }
                }
            }
        }

        const auto keep_recent = static_cast<std::size_t>(time_based_microcompact_keep_recent());
        if (compactable_ids.size() <= keep_recent) return;

        std::unordered_set<std::string> keep_ids;
        const auto keep_start = compactable_ids.size() - keep_recent;
        for (std::size_t i = keep_start; i < compactable_ids.size(); ++i) {
            keep_ids.insert(compactable_ids[i]);
        }

        std::unordered_set<std::string> clear_ids;
        for (const auto& id : compactable_ids) {
            if (!keep_ids.contains(id)) clear_ids.insert(id);
        }
        if (clear_ids.empty()) return;

        bool changed = false;
        for (auto& message : conversation_) {
            auto* result = std::get_if<ToolResultMessage>(&message);
            if (!result || !clear_ids.contains(result->tool_use_id.value)) continue;
            auto existing_text = tool_result_plain_text_content(*result);
            if (existing_text && *existing_text == cc::utils::TOOL_RESULT_CLEARED_MESSAGE) {
                continue;
            }
            result->content.clear();
            result->content.push_back(TextBlock{std::string(cc::utils::TOOL_RESULT_CLEARED_MESSAGE)});
            changed = true;
        }

        if (changed) {
            rebuild_content_replacement_state_locked();
        }
    }

    [[nodiscard]] std::filesystem::path query_tool_result_path(std::string_view tool_use_id) const {
        return cc::tools::agent_runtime::runtime_state_dir() /
            "tool-results" /
            (cc::tools::agent_runtime::safe_agent_filename(session_id_.str()) + "-" +
                cc::tools::agent_runtime::safe_agent_filename(tool_use_id) + ".txt");
    }

    [[nodiscard]] std::optional<std::string> build_query_tool_result_replacement(
        const QueryToolResultBudgetCandidate& candidate,
        std::string_view content) const {
        auto path = query_tool_result_path(candidate.tool_use_id);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return std::nullopt;
        if (!std::filesystem::exists(path, ec)) {
            std::ofstream out(path, std::ios::trunc);
            if (!out) return std::nullopt;
            out << content;
            if (!out.good()) return std::nullopt;
        }

        constexpr std::size_t preview_size = 2'000;
        const auto preview_len = std::min(preview_size, content.size());
        std::string replacement;
        replacement.reserve(preview_len + path.string().size() + 192);
        replacement += cc::utils::PERSISTED_OUTPUT_TAG;
        replacement += "\n";
        replacement += std::format(
            "Output too large ({} bytes). Full output saved to: {}\n\n",
            content.size(),
            path.string());
        replacement += std::format("Preview (first {} bytes):\n", preview_len);
        replacement += content.substr(0, preview_len);
        replacement += content.size() > preview_len ? "\n...\n" : "\n";
        replacement += cc::utils::PERSISTED_OUTPUT_CLOSING_TAG;
        return replacement;
    }

    void replace_tool_result_message_locked(std::size_t message_index, std::string replacement) {
        auto* tool_result = std::get_if<ToolResultMessage>(&conversation_[message_index]);
        if (!tool_result) return;
        tool_result->content.clear();
        tool_result->content.push_back(TextBlock{std::move(replacement)});
    }

    void rebuild_content_replacement_state_locked() {
        content_replacement_seen_ids_.clear();
        content_replacements_.clear();
        for (const auto& message : conversation_) {
            const auto* tool_result = std::get_if<ToolResultMessage>(&message);
            if (!tool_result || tool_result->tool_use_id.value.empty()) continue;
            auto text = tool_result_plain_text_content(*tool_result);
            if (!text) continue;
            content_replacement_seen_ids_.insert(tool_result->tool_use_id.value);
            if (tool_result_already_replaced(*text)) {
                content_replacements_[tool_result->tool_use_id.value] = std::move(*text);
            }
        }
    }

    void apply_tool_result_budget() {
        std::lock_guard lock(conversation_mutex_);
        if (conversation_.empty()) return;

        static constexpr std::size_t max_tool_results_per_message_chars = 200'000;
        const auto tool_names = tool_name_by_tool_use_id(conversation_);
        const auto skip_tool_names = unbounded_tool_result_budget_names();

        for (const auto& candidates : collect_tool_result_budget_candidates_by_message(conversation_, tool_names)) {
            std::vector<QueryToolResultBudgetCandidate> fresh;
            std::size_t frozen_size = 0;
            std::size_t fresh_size = 0;

            for (const auto& candidate : candidates) {
                if (auto replacement = content_replacements_.find(candidate.tool_use_id);
                    replacement != content_replacements_.end()) {
                    replace_tool_result_message_locked(candidate.message_index, replacement->second);
                    content_replacement_seen_ids_.insert(candidate.tool_use_id);
                } else if (content_replacement_seen_ids_.contains(candidate.tool_use_id)) {
                    frozen_size += candidate.size;
                } else if (!candidate.tool_name.empty() &&
                           skip_tool_names.contains(lowercase_ascii(candidate.tool_name))) {
                    content_replacement_seen_ids_.insert(candidate.tool_use_id);
                } else {
                    fresh.push_back(candidate);
                    fresh_size += candidate.size;
                }
            }

            if (fresh.empty()) continue;
            std::vector<QueryToolResultBudgetCandidate> selected;
            if (frozen_size + fresh_size > max_tool_results_per_message_chars) {
                std::ranges::sort(fresh, {}, &QueryToolResultBudgetCandidate::size);
                std::ranges::reverse(fresh);
                auto remaining = frozen_size + fresh_size;
                for (const auto& candidate : fresh) {
                    if (remaining <= max_tool_results_per_message_chars) break;
                    selected.push_back(candidate);
                    remaining -= candidate.size;
                }
            }

            std::unordered_set<std::string> selected_ids;
            for (const auto& candidate : selected) {
                selected_ids.insert(candidate.tool_use_id);
            }
            for (const auto& candidate : fresh) {
                if (!selected_ids.contains(candidate.tool_use_id)) {
                    content_replacement_seen_ids_.insert(candidate.tool_use_id);
                }
            }

            for (const auto& candidate : selected) {
                auto* tool_result = std::get_if<ToolResultMessage>(&conversation_[candidate.message_index]);
                if (!tool_result) continue;
                auto text = tool_result_text_for_budget(*tool_result);
                content_replacement_seen_ids_.insert(candidate.tool_use_id);
                if (!text) continue;
                auto replacement = build_query_tool_result_replacement(candidate, *text);
                if (!replacement) continue;
                replace_tool_result_message_locked(candidate.message_index, *replacement);
                content_replacements_[candidate.tool_use_id] = std::move(*replacement);
            }
        }
    }

    [[nodiscard]] static std::string truncate_for_compaction(std::string text,
                                                             std::size_t limit) {
        if (text.size() <= limit) return text;
        text.resize(limit);
        text += "...";
        return text;
    }

    [[nodiscard]] static std::string content_block_compaction_text(const ContentBlock& block) {
        return std::visit([](const auto& b) -> std::string {
            using T = std::remove_cvref_t<decltype(b)>;
            if constexpr (std::same_as<T, TextBlock>) {
                return b.text;
            } else if constexpr (std::same_as<T, ToolUseBlock>) {
                return std::format("[tool_use:{} {}]", b.name, b.input_json);
            } else if constexpr (std::same_as<T, ToolResultBlock>) {
                return std::format("[tool_result:{} error={}] {}",
                    b.tool_use_id.value,
                    b.is_error ? "true" : "false",
                    b.content);
            } else if constexpr (std::same_as<T, ImageBlock>) {
                return std::format("[image:{} {} bytes]", b.media_type, b.data.size());
            } else if constexpr (std::same_as<T, DocumentBlock>) {
                return std::format("[document:{} {} bytes]", b.media_type, b.data.size());
            } else if constexpr (std::same_as<T, ThinkingBlock>) {
                return "[thinking omitted from compact summary]";
            } else {
                return "[unknown content]";
            }
        }, block);
    }

    [[nodiscard]] static std::string message_compaction_text(const Message& message) {
        std::string text;
        std::visit([&](const auto& m) {
            for (const auto& block : m.content) {
                auto block_text = content_block_compaction_text(block);
                if (block_text.empty()) continue;
                if (!text.empty()) text += "\n";
                text += std::move(block_text);
            }
        }, message);
        if (text.empty()) return "[empty message]";
        return truncate_for_compaction(std::move(text), 300);
    }

    [[nodiscard]] static bool is_compact_boundary_message(const Message& message) {
        const auto* system = std::get_if<SystemMessage>(&message);
        return system && system->subtype == "compact_boundary";
    }

    [[nodiscard]] static bool is_snip_boundary_message(const Message& message) {
        const auto* system = std::get_if<SystemMessage>(&message);
        return system && system->subtype == "snip_boundary" && system->snip_metadata.has_value();
    }

    void replay_snip_boundaries() {
        std::lock_guard lock(conversation_mutex_);
        std::unordered_set<std::string> removed_ids;
        for (const auto& message : conversation_) {
            const auto* system = std::get_if<SystemMessage>(&message);
            if (!system || !system->snip_metadata) continue;
            for (const auto& uuid : system->snip_metadata->removed_uuids) {
                if (!uuid.empty()) removed_ids.insert(uuid);
            }
        }
        if (removed_ids.empty()) return;

        auto should_remove = [&](const Message& message) {
            const auto id = message_id_value(message);
            const bool protected_system =
                std::holds_alternative<SystemMessage>(message) && !is_snip_boundary_message(message);
            return removed_ids.contains(id) && !protected_system;
        };
        if (!std::ranges::any_of(conversation_, should_remove)) return;

        std::vector<Message> projected;
        projected.reserve(conversation_.size());
        for (auto& message : conversation_) {
            if (should_remove(message)) continue;
            projected.push_back(std::move(message));
        }

        conversation_ = std::move(projected);
        rebuild_content_replacement_state_locked();
    }

    [[nodiscard]] static std::string message_id_value(const Message& message) {
        return std::visit([](const auto& value) {
            return value.id.value;
        }, message);
    }

    [[nodiscard]] static std::optional<CompactPreservedSegment> compact_preserved_segment(
        std::vector<Message>::const_iterator first,
        std::vector<Message>::const_iterator last,
        std::string_view anchor_id) {
        if (first == last) return std::nullopt;
        return CompactPreservedSegment{
            .head_uuid = message_id_value(*first),
            .anchor_uuid = std::string(anchor_id),
            .tail_uuid = message_id_value(*(last - 1)),
        };
    }

    [[nodiscard]] static std::string build_compaction_summary(
        std::vector<Message>::const_iterator first,
        std::vector<Message>::const_iterator last) {
        const auto count = static_cast<std::size_t>(std::ranges::count_if(
            first,
            last,
            [](const Message& message) {
                return !is_compact_boundary_message(message);
            }));
        std::string summary = std::format(
            "[Conversation compacted: {} older messages summarized]\n"
            "Preserve these details from the compacted history:",
            count);
        constexpr std::size_t max_summary_chars = 8000;
        std::size_t index = 1;
        for (auto it = first; it != last; ++it) {
            if (is_compact_boundary_message(*it)) continue;
            auto line = std::format("\n- {} #{}: {}",
                role_to_string(get_role(*it)),
                index++,
                message_compaction_text(*it));
            if (summary.size() + line.size() > max_summary_chars) {
                summary += "\n- [remaining compacted messages omitted due to summary size limit]";
                break;
            }
            summary += std::move(line);
        }
        return summary;
    }

    /// Tool loop result structure
    struct ToolLoopResult {
        AssistantMessage message;
        TokenUsage usage;
        std::uint32_t rounds;
    };

    /// Execute the full tool-call loop until completion or abort
    [[nodiscard]] Result<ToolLoopResult> execute_tool_loop(const QueryOptions& options) {
        TokenUsage loop_usage{};
        std::uint32_t round = 0;
        std::uint32_t max_tokens_retries = 0;
        const std::uint32_t max_rounds = options.max_tool_rounds.value_or(20);

        while (round < max_rounds && !should_abort() && !budget_tracker_.budget_exceeded) {
            // Check stop hooks before each iteration
            if (lifecycle_hooks_) {
                if (auto stop_reason = lifecycle_hooks_->check_stop_hooks()) {
                    return std::unexpected(Error::make(
                        ErrorCode::InternalError,
                        std::format("Query stopped by hook: {}", *stop_reason)));
                }
            }

            append_pending_native_agent_notifications();
            auto api_result = call_api(options);
            if (!api_result) return std::unexpected(api_result.error());

            auto& [msg, usage] = *api_result;
            loop_usage += usage;
            cumulative_usage_ += usage;

            // Track budget
            budget_tracker_.add_usage(usage, config_.model_params.model, model_cost_);
            if (budget_tracker_.budget_exceeded) {
                return std::unexpected(Error::make(
                    ErrorCode::ContextWindowExceeded,
                    "Budget limit exceeded"));
            }

            // P1-3: Max output tokens recovery
            if (msg.stop_reason == "max_tokens" && max_tokens_retries < 3) {
                // Escalate max_tokens to 64k
                config_.model_params.max_tokens = std::max(config_.model_params.max_tokens, std::uint32_t{64000});
                // Append partial response and a resume nudge
                append_message(Message{msg});
                auto resume = make_user_message(
                    "Your previous response was cut off at the token limit. "
                    "Please continue from where you left off.");
                append_message(Message{std::move(resume)});
                ++max_tokens_retries;
                ++round;
                continue;
            }

            // Check if response contains tool_use blocks
            bool has_tool_use = false;
            for (const auto& block : msg.content) {
                if (std::holds_alternative<ToolUseBlock>(block)) {
                    has_tool_use = true;
                    break;
                }
            }

            if (!has_tool_use) {
                // No more tool calls - return final response
                return ToolLoopResult{std::move(msg), loop_usage, round};
            }

            // Execute tools and append results
            append_message(Message{msg});
            auto results = execute_pending_tools(msg, options);
            for (auto& r : results) {
                append_message(Message{std::move(r)});
            }
            ++round;
        }

        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            std::format("Tool loop exceeded maximum rounds ({})", max_rounds)));
    }

    /// Single API call result
    struct ApiCallResult {
        AssistantMessage message;
        TokenUsage usage;
    };

    /// Execute a single API call with retry logic
    [[nodiscard]] Result<ApiCallResult> call_api(const QueryOptions& options) {
        auto& policy = config_.retry_policy;
        auto delay = policy.initial_delay;
        std::size_t fallback_idx = 0;

        for (std::uint32_t attempt = 0; attempt <= policy.max_retries; ++attempt) {
            if (should_abort()) {
                return std::unexpected(Error::make(
                    ErrorCode::InternalError,
                    "Query aborted"));
            }

            auto result = send_request(options);
            if (should_abort()) {
                return std::unexpected(Error::make(
                    ErrorCode::InternalError,
                    "Query aborted"));
            }
            if (result) return *result;

            // Determine if error is retryable
            auto& err = result.error();
            bool retryable = is_retryable_error(err.code);

            // P1-4: Model fallback on capacity/overload errors
            if ((err.code == ErrorCode::OverloadedError || err.code == ErrorCode::RateLimited) &&
                fallback_idx < config_.fallback_models.size()) {
                config_.model_params.model = config_.fallback_models[fallback_idx++];
                continue;  // Retry immediately with fallback model
            }

            if (!retryable || attempt == policy.max_retries) {
                return std::unexpected(err);
            }

            // Add jitter to prevent thundering herd
            auto jittered_delay = add_jitter(delay);
            std::this_thread::sleep_for(jittered_delay);

            // Exponential backoff with cap
            delay = std::chrono::milliseconds(
                std::min<long long>(
                    static_cast<long long>(delay.count() * policy.backoff_multiplier),
                    policy.max_delay.count()));
        }

        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            "Retry logic exhausted"));
    }

    /// Check if error code is retryable
    [[nodiscard]] bool is_retryable_error(ErrorCode code) const {
        return code == ErrorCode::ConnectionFailed ||
               code == ErrorCode::NetworkTimeout ||
               code == ErrorCode::OverloadedError ||
               code == ErrorCode::RateLimited;
    }

    /// Check whether a tool is enabled for the current query.
    [[nodiscard]] static bool is_tool_enabled_for_query(
        std::string_view tool_name,
        const QueryOptions& options) {
        if (options.enabled_tools.empty()) return true;
        return std::ranges::any_of(options.enabled_tools, [tool_name](const std::string& enabled) {
            return enabled == tool_name;
        });
    }

    [[nodiscard]] std::optional<cc::services::compact::ContextManagementConfig>
    api_context_management() const {
        return cc::services::compact::get_api_context_management({
            .has_thinking = thinking_enabled_for_request(),
            .is_redact_thinking_active = false,
            .clear_all_thinking = false,
        });
    }

    [[nodiscard]] bool thinking_enabled_for_request() const {
        return config_.thinking_config.mode != ThinkingConfig::Mode::Disabled &&
            !cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_DISABLE_THINKING"));
    }

    void add_input_tokens_object(
        cc::utils::json::JsonMutVal& parent,
        cc::utils::json::JsonMutDoc& doc,
        std::string_view key,
        std::uint32_t value
    ) const {
        auto obj = doc.object();
        obj.add("type", doc.string("input_tokens"));
        obj.add("value", doc.number(static_cast<int64_t>(value)));
        parent.add(key, obj);
    }

    void add_string_array(
        cc::utils::json::JsonMutVal& parent,
        cc::utils::json::JsonMutDoc& doc,
        std::string_view key,
        const std::vector<std::string>& values
    ) const {
        auto arr = doc.array();
        for (const auto& value : values) {
            arr.append(doc.string(value));
        }
        parent.add(key, arr);
    }

    void add_context_management_to_json(
        cc::utils::json::JsonMutVal& root,
        cc::utils::json::JsonMutDoc& doc,
        const cc::services::compact::ContextManagementConfig& config
    ) const {
        auto context_management = doc.object();
        auto edits = doc.array();

        for (const auto& edit : config.edits) {
            auto obj = doc.object();
            obj.add("type", doc.string(edit.type));

            if (edit.trigger_input_tokens) {
                add_input_tokens_object(obj, doc, "trigger", *edit.trigger_input_tokens);
            }
            if (edit.clear_at_least_input_tokens) {
                add_input_tokens_object(obj, doc, "clear_at_least", *edit.clear_at_least_input_tokens);
            }
            if (edit.has_thinking_keep) {
                if (edit.keep_all_thinking) {
                    obj.add("keep", doc.string("all"));
                } else if (edit.keep_thinking_turns) {
                    auto keep = doc.object();
                    keep.add("type", doc.string("thinking_turns"));
                    keep.add("value", doc.number(static_cast<int64_t>(*edit.keep_thinking_turns)));
                    obj.add("keep", keep);
                }
            } else if (edit.keep_tool_uses) {
                auto keep = doc.object();
                keep.add("type", doc.string("tool_uses"));
                keep.add("value", doc.number(static_cast<int64_t>(*edit.keep_tool_uses)));
                obj.add("keep", keep);
            }
            if (!edit.clear_tool_inputs.empty()) {
                add_string_array(obj, doc, "clear_tool_inputs", edit.clear_tool_inputs);
            }
            if (!edit.exclude_tools.empty()) {
                add_string_array(obj, doc, "exclude_tools", edit.exclude_tools);
            }

            edits.append(obj);
        }

        context_management.add("edits", edits);
        root.add("context_management", context_management);
    }

    void add_output_config_to_json(
        cc::utils::json::JsonMutVal& root,
        cc::utils::json::JsonMutDoc& doc
    ) const {
        const bool has_budget = config_.task_budget.has_value();
        const bool has_schema = config_.response_schema.has_value();
        if (!has_budget && !has_schema) return;

        auto output_config = doc.object();
        if (has_budget) {
            auto task_budget = doc.object();
            task_budget.add("type", doc.string("tokens"));
            task_budget.add("total", doc.number(static_cast<int64_t>(config_.task_budget->total)));
            if (config_.task_budget->remaining) {
                task_budget.add(
                    "remaining",
                    doc.number(static_cast<int64_t>(*config_.task_budget->remaining)));
            }
            output_config.add("task_budget", task_budget);
        }
        if (has_schema) {
            // Structured output: force the model to return JSON conforming to
            // the supplied JSON schema (Anthropic output_config.format.json_schema).
            auto format = doc.object();
            format.add("type", doc.string("json_schema"));
            auto schema_obj = doc.object();
            schema_obj.add("name", doc.string(config_.response_schema->name));
            auto schema_val = doc.raw_json(config_.response_schema->schema_json);
            if (schema_val.raw()) {
                schema_obj.add("schema", schema_val);
            }
            format.add("json_schema", schema_obj);
            output_config.add("format", format);
        }
        root.add("output_config", output_config);
    }

public:
    /// Build the output_config JSON fragment (for testing / introspection).
    /// Returns "{}" when neither task_budget nor response_schema is configured.
    [[nodiscard]] std::string build_output_config_json_for_testing() const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        add_output_config_to_json(root, doc);
        doc.set_root(root);
        return doc.to_string();
    }

private:

    void update_task_budget_remaining_after_compact(std::uint32_t pre_compact_tokens) {
        if (!config_.task_budget) return;
        const auto current = config_.task_budget->remaining.value_or(config_.task_budget->total);
        config_.task_budget->remaining =
            pre_compact_tokens >= current ? 0 : current - pre_compact_tokens;
    }

    void restore_task_budget_remaining_from_compact_boundaries_locked() {
        if (!config_.task_budget || config_.task_budget->remaining) return;

        std::uint64_t compacted_tokens = 0;
        for (const auto& message : conversation_) {
            const auto* system = std::get_if<SystemMessage>(&message);
            if (!system || !system->subtype || *system->subtype != "compact_boundary" ||
                !system->compact_metadata) {
                continue;
            }
            compacted_tokens += system->compact_metadata->pre_tokens;
            if (compacted_tokens >= config_.task_budget->total) {
                config_.task_budget->remaining = 0;
                return;
            }
        }

        if (compacted_tokens > 0) {
            config_.task_budget->remaining =
                config_.task_budget->total - static_cast<std::uint32_t>(compacted_tokens);
        }
    }

    void add_beta_headers(httplib::Headers& headers) const {
        if (config_.task_budget) {
            headers.emplace("anthropic-beta", "task-budgets-2026-03-13");
        }
        if (api_context_management()) {
            headers.emplace("anthropic-beta", "context-management-2025-06-27");
        }
    }

    /// Build HTTP request body
    [[nodiscard]] std::string build_request_body(
        const QueryOptions& options,
        bool stream = false) const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();

        root.add("model", doc.string(config_.model_params.model));
        root.add("max_tokens", doc.number(static_cast<int64_t>(config_.model_params.max_tokens)));
        root.add("stream", doc.boolean(stream));

        // Extract system prompt and build messages array
        std::string system_prompt_text;
        auto messages_arr = doc.array();
        {
            std::lock_guard lock(conversation_mutex_);
            std::unordered_set<std::string> snipped_message_ids;
            for (const auto& msg : conversation_) {
                const auto* sys = std::get_if<SystemMessage>(&msg);
                if (!sys || !sys->snip_metadata) continue;
                for (const auto& uuid : sys->snip_metadata->removed_uuids) {
                    snipped_message_ids.insert(uuid);
                }
            }

            for (const auto& msg : conversation_) {
                const auto message_id = std::visit(
                    [](const auto& value) { return value.id.value; },
                    msg);
                if (snipped_message_ids.contains(message_id)) {
                    continue;
                }

                if (const auto* sys = std::get_if<SystemMessage>(&msg)) {
                    if (sys->subtype == "compact_boundary") {
                        continue;
                    }
                    // Extract system prompt from first non-boundary SystemMessage.
                    if (system_prompt_text.empty()) {
                        for (const auto& block : sys->content) {
                            if (const auto* tb = std::get_if<TextBlock>(&block)) {
                                system_prompt_text += tb->text;
                            }
                        }
                    }
                    continue; // Don't add to messages array
                }
                append_message_to_json(msg, messages_arr, doc);
            }
        }

        // Add system prompt as top-level field (Anthropic API format)
        if (!system_prompt_text.empty()) {
            root.add("system", doc.string(system_prompt_text));
        }

        root.add("messages", messages_arr);

        // Build tools array with input_schema as JSON object
        if (!config_.tools.empty()) {
            auto tools_arr = doc.array();
            std::size_t enabled_tool_count = 0;
            for (const auto& tool : config_.tools) {
                if (!is_tool_enabled_for_query(tool.name, options)) continue;
                ++enabled_tool_count;
                auto tool_obj = doc.object();
                tool_obj.add("name", doc.string(tool.name));
                tool_obj.add("description", doc.string(tool.description));
                // Parse input_schema JSON string into a proper JSON object
                auto schema_json = tool.input_schema.to_json();
                auto schema_doc = cc::utils::json::parse(schema_json);
                if (schema_doc) {
                    tool_obj.add("input_schema", doc.copy_val(schema_doc->root()));
                } else {
                    // Fallback: embed as raw object string
                    tool_obj.add("input_schema", doc.raw_json(schema_json));
                }
                tools_arr.append(tool_obj);
            }
            if (enabled_tool_count > 0) {
                root.add("tools", tools_arr);
            }
        }

        // Extended thinking configuration
        if (thinking_enabled_for_request()) {
            auto thinking_obj = doc.object();
            if (config_.thinking_config.mode == ThinkingConfig::Mode::Adaptive) {
                thinking_obj.add("type", doc.string("adaptive"));
            } else {
                thinking_obj.add("type", doc.string("enabled"));
                auto budget = config_.thinking_config.budget_tokens.value_or(10000);
                thinking_obj.add("budget_tokens", doc.number(static_cast<int64_t>(budget)));
            }
            root.add("thinking", thinking_obj);
        }

        // Optional parameters
        if (config_.model_params.temperature) {
            root.add("temperature", doc.number(*config_.model_params.temperature));
        }
        if (config_.model_params.top_p) {
            root.add("top_p", doc.number(*config_.model_params.top_p));
        }
        if (config_.model_params.top_k) {
            root.add("top_k", doc.number(static_cast<int64_t>(*config_.model_params.top_k)));
        }

        if (auto context_management = api_context_management()) {
            add_context_management_to_json(root, doc, *context_management);
        }
        add_output_config_to_json(root, doc);

        doc.set_root(root);
        return doc.to_string();
    }

    /// Append a Message variant to JSON array
    void append_message_to_json(const Message& msg,
                                cc::utils::json::JsonMutVal& arr,
                                cc::utils::json::JsonMutDoc& doc) const {
        std::visit([&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            auto msg_obj = doc.object();

            if constexpr (std::is_same_v<T, UserMessage>) {
                msg_obj.add("role", doc.string("user"));
                msg_obj.add("content", content_to_json(m.content, doc));
            } else if constexpr (std::is_same_v<T, AssistantMessage>) {
                msg_obj.add("role", doc.string("assistant"));
                msg_obj.add("content", content_to_json(m.content, doc));
            } else if constexpr (std::is_same_v<T, SystemMessage>) {
                // System messages are handled separately
                return;
            } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
                msg_obj.add("role", doc.string("user"));
                auto result_obj = doc.object();
                result_obj.add("type", doc.string("tool_result"));
                result_obj.add("tool_use_id", doc.string(m.tool_use_id.value));
                result_obj.add("is_error", doc.boolean(m.is_error));

                std::string content_str;
                bool has_rich_content = false;
                for (const auto& block : m.content) {
                    if (const auto* text = std::get_if<TextBlock>(&block)) {
                        content_str += text->text;
                    } else if (const auto* image = std::get_if<ImageBlock>(&block)) {
                        (void)image;
                        has_rich_content = true;
                    }
                }
                if (has_rich_content) {
                    auto rich_content = doc.array();
                    if (!content_str.empty()) {
                        auto text_obj = doc.object();
                        text_obj.add("type", doc.string("text"));
                        text_obj.add("text", doc.string(content_str));
                        rich_content.append(text_obj);
                    }
                    for (const auto& block : m.content) {
                        if (const auto* image = std::get_if<ImageBlock>(&block)) {
                            auto image_obj = doc.object();
                            image_obj.add("type", doc.string("image"));
                            auto source = doc.object();
                            source.add("type", doc.string("base64"));
                            source.add("media_type", doc.string(image->media_type));
                            source.add("data", doc.string(image->data));
                            image_obj.add("source", source);
                            rich_content.append(image_obj);
                        }
                    }
                    result_obj.add("content", rich_content);
                } else {
                    result_obj.add("content", doc.string(content_str));
                }
                auto content_arr = doc.array();
                content_arr.append(result_obj);
                for (const auto& block : m.content) {
                    if (const auto* document = std::get_if<DocumentBlock>(&block)) {
                        auto document_obj = doc.object();
                        document_obj.add("type", doc.string("document"));
                        auto source = doc.object();
                        source.add("type", doc.string("base64"));
                        source.add("media_type", doc.string(document->media_type));
                        source.add("data", doc.string(document->data));
                        document_obj.add("source", source);
                        content_arr.append(document_obj);
                    }
                }
                msg_obj.add("content", content_arr);
            }

            arr.append(msg_obj);
        }, msg);
    }

    /// Convert content blocks to JSON
    [[nodiscard]] cc::utils::json::JsonMutVal content_to_json(
        const std::vector<ContentBlock>& content,
        cc::utils::json::JsonMutDoc& doc) const {
        if (content.size() == 1) {
            if (const auto* text = std::get_if<TextBlock>(&content[0])) {
                return doc.string(text->text);
            }
        }

        auto arr = doc.array();
        for (const auto& block : content) {
            std::visit([&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("text"));
                    obj.add("text", doc.string(b.text));
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("tool_use"));
                    obj.add("id", doc.string(b.id.value));
                    obj.add("name", doc.string(b.name));
                    // Input must be a JSON object, not a string
                    auto input_doc = cc::utils::json::parse(b.input_json);
                    if (input_doc) {
                        obj.add("input", doc.copy_val(input_doc->root()));
                    } else {
                        obj.add("input", doc.raw_json(b.input_json));
                    }
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("thinking"));
                    obj.add("thinking", doc.string(b.thinking));
                    obj.add("signature", doc.string(b.signature));
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ImageBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("image"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(b.media_type));
                    source.add("data", doc.string(b.data));
                    obj.add("source", source);
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, DocumentBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("document"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(b.media_type));
                    source.add("data", doc.string(b.data));
                    obj.add("source", source);
                    arr.append(obj);
                }
            }, block);
        }
        return arr;
    }

    /// Low-level API request using httplib
    [[nodiscard]] Result<ApiCallResult> send_request(
        const QueryOptions& options,
        bool is_retry_after_compact = false) {
        replay_snip_boundaries();
        apply_time_based_microcompact();
        apply_tool_result_budget();

        auto endpoint = api_messages_endpoint(api_config_.base_url);
        if (!endpoint) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                endpoint.error()));
        }

        // Build headers
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("anthropic-version", api_config_.api_version);
        if (!api_config_.auth_token.empty()) {
            headers.emplace("Authorization", std::format("Bearer {}", api_config_.auth_token));
        } else {
            headers.emplace("x-api-key", api_config_.api_key);
        }
        headers.emplace("User-Agent", "CC-REPL/1.0");
        add_beta_headers(headers);

        // Build request body
        std::string body = build_request_body(options);

        // Create HTTP client
        httplib::Client cli(endpoint->client_base_url);
        cli.set_connection_timeout(api_config_.timeout);
        cli.set_read_timeout(api_config_.timeout);
        cli.set_write_timeout(api_config_.timeout);

        // Send request
        auto res = cli.Post(endpoint->path, headers, body, "application/json");

        if (!res) {
            auto err = res.error();
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed,
                std::format("HTTP request failed: {}", httplib::to_string(err))));
        }

        // Check for HTTP errors
        if (res->status >= 400) {
            // P1-2: Reactive compact on 413 / prompt-too-long
            if (!is_retry_after_compact &&
                (res->status == 413 ||
                 res->body.find("prompt_too_long") != std::string::npos ||
                 res->body.find("Prompt is too long") != std::string::npos)) {
                auto compact_result = compact_conversation("reactive");
                if (compact_result) {
                    // Retry once after compaction
                    return send_request(options, /*is_retry_after_compact=*/true);
                }
            }

            return std::unexpected(Error::make(
                classify_http_error(res->status),
                std::format("API error ({}): {}", res->status, res->body)));
        }

        // Parse response
        return parse_api_response(res->body);
    }

    /// Classify HTTP status code to error code
    [[nodiscard]] ErrorCode classify_http_error(int status) const {
        if (status == 401 || status == 403) {
            return ErrorCode::AuthenticationFailed;
        } else if (status == 429) {
            return ErrorCode::RateLimited;
        } else if (status == 529) {
            return ErrorCode::OverloadedError;
        } else if (status >= 400 && status < 500) {
            return ErrorCode::InvalidRequest;
        } else if (status >= 500) {
            return ErrorCode::InternalError;
        }
        return ErrorCode::InternalError;
    }

    /// Parse API response to our types
    [[nodiscard]] Result<ApiCallResult> parse_api_response(std::string_view response_body) {
        auto doc_result = cc::utils::json::parse(response_body);
        if (!doc_result) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Failed to parse API response"));
        }

        auto root = doc_result->root();

        ApiCallResult result;

        // Build assistant message
        result.message.id.value = std::string(root.get("id").as_str());
        result.message.model = std::string(root.get("model").as_str());
        result.message.timestamp = std::chrono::system_clock::now();

        // Parse stop reason
        auto stop_reason = root.get("stop_reason");
        if (stop_reason.valid() && !stop_reason.is_null()) {
            result.message.stop_reason = std::string(stop_reason.as_str());
        }

        // Parse content
        auto content = root.get("content");
        if (content.valid() && content.is_arr()) {
            content.iter([&](cc::utils::json::JsonVal block) {
                parse_content_block(block, result.message.content);
            });
        }

        // Parse usage
        auto usage = root.get("usage");
        if (usage.valid() && usage.is_obj()) {
            result.usage.input_tokens = static_cast<std::uint32_t>(usage.get("input_tokens").as_int());
            result.usage.output_tokens = static_cast<std::uint32_t>(usage.get("output_tokens").as_int());
            auto cache_creation = usage.get("cache_creation_input_tokens");
            if (cache_creation.valid()) {
                result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
            }
            auto cache_read = usage.get("cache_read_input_tokens");
            if (cache_read.valid()) {
                result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
            }
        }

        return result;
    }

    /// Parse a single content block from JSON
    void parse_content_block(cc::utils::json::JsonVal block,
                            std::vector<ContentBlock>& content) const {
        auto type = block.get("type").as_str();

        if (type == "text") {
            TextBlock tb;
            tb.text = std::string(block.get("text").as_str());
            content.push_back(std::move(tb));
        } else if (type == "tool_use") {
            ToolUseBlock tub;
            tub.id.value = std::string(block.get("id").as_str());
            tub.name = std::string(block.get("name").as_str());
            auto input = block.get("input");
            tub.input_json = input.valid() ? cc::utils::json::to_string(input) : "{}";
            content.push_back(std::move(tub));
        } else if (type == "thinking") {
            ThinkingBlock tb;
            tb.thinking = std::string(block.get("thinking").as_str());
            auto signature = block.get("signature");
            if (signature.valid()) {
                tb.signature = std::string(signature.as_str());
            }
            content.push_back(std::move(tb));
        }
    }

    /// Single streaming API call result
    struct StreamCallResult {
        AssistantMessage message;
        TokenUsage usage;
        bool has_tool_use = false;
        bool failed = false;
        std::string error_message;
    };

    /// Stream a single API call with event callbacks
    [[nodiscard]] StreamCallResult stream_single_api_call(const QueryOptions& options) {
        StreamCallResult result;
        replay_snip_boundaries();
        apply_time_based_microcompact();
        apply_tool_result_budget();

        // Build headers
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("anthropic-version", api_config_.api_version);
        if (!api_config_.auth_token.empty()) {
            headers.emplace("Authorization", std::format("Bearer {}", api_config_.auth_token));
        } else {
            headers.emplace("x-api-key", api_config_.api_key);
        }
        headers.emplace("User-Agent", "CC-REPL/1.0");
        add_beta_headers(headers);

        // Build streaming request body
        std::string body = build_request_body(options, /*stream=*/true);

        auto endpoint = api_messages_endpoint(api_config_.base_url);
        if (!endpoint) {
            result.failed = true;
            result.error_message = endpoint.error();
            if (options.on_event) {
                StreamError ev;
                ev.error_type = "invalid_request_error";
                ev.message = result.error_message;
                (*options.on_event)(ev);
            }
            return result;
        }

        // Create HTTP client
        httplib::Client cli(endpoint->client_base_url);
        cli.set_connection_timeout(api_config_.timeout);
        cli.set_read_timeout(api_config_.timeout);
        cli.set_write_timeout(api_config_.timeout);

        // SSE parsing state
        SseEventDecoder sse_decoder_;
        std::uint32_t block_index = 0;

        // Content accumulation
        struct BlockAccum {
            ContentBlock block;
            std::string accumulated_text;
            std::string accumulated_json;
        };
        std::vector<BlockAccum> blocks;

        auto emit_stream_error = [&](std::string error_type, std::string message) {
            result.failed = true;
            result.error_message = message.empty() ? error_type : message;
            if (options.on_event) {
                StreamError ev;
                ev.error_type = std::move(error_type);
                ev.message = result.error_message;
                (*options.on_event)(ev);
            }
        };

        auto parse_sse_event = [&](const std::string& event_type,
                                    const std::string& data) {
            if (data.empty() || data == "[DONE]") return;

            auto doc_result = cc::utils::json::parse(data);
            if (!doc_result) return;
            auto root = doc_result->root();

            if (event_type == "message_start") {
                auto msg = root.get("message");
                if (msg.valid()) {
                    auto id = msg.get("id");
                    if (id.valid()) result.message.id.value = std::string(id.as_str());
                    auto model = msg.get("model");
                    if (model.valid()) result.message.model = std::string(model.as_str());
                    auto usage = msg.get("usage");
                    if (usage.valid() && usage.is_obj()) {
                        auto input = usage.get("input_tokens");
                        if (input.valid()) {
                            result.usage.input_tokens = static_cast<std::uint32_t>(input.as_int());
                        }
                        auto cache_creation = usage.get("cache_creation_input_tokens");
                        if (cache_creation.valid()) {
                            result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                        }
                        auto cache_read = usage.get("cache_read_input_tokens");
                        if (cache_read.valid()) {
                            result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                        }
                    }
                }
                if (options.on_event) {
                    StreamStart ev;
                    ev.message_id = result.message.id;
                    ev.model = result.message.model.value_or(config_.model_params.model);
                    (*options.on_event)(ev);
                }
            } else if (event_type == "content_block_start") {
                auto idx = root.get("index");
                block_index = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : static_cast<std::uint32_t>(blocks.size());
                auto cb = root.get("content_block");
                BlockAccum accum;
                if (cb.valid()) {
                    auto type = cb.get("type");
                    if (type.valid()) {
                        auto type_str = type.as_str();
                        if (type_str == "text") {
                            accum.block = TextBlock{};
                        } else if (type_str == "tool_use") {
                            ToolUseBlock tub;
                            auto id = cb.get("id");
                            if (id.valid()) tub.id.value = std::string(id.as_str());
                            auto name = cb.get("name");
                            if (name.valid()) tub.name = std::string(name.as_str());
                            accum.block = std::move(tub);
                        } else if (type_str == "thinking") {
                            accum.block = ThinkingBlock{};
                        }
                    }
                }
                blocks.push_back(std::move(accum));
                if (options.on_event) {
                    ContentBlockStart ev;
                    ev.index = block_index;
                    ev.block = blocks.back().block;
                    (*options.on_event)(ev);
                }
            } else if (event_type == "content_block_delta") {
                auto idx = root.get("index");
                auto cur_idx = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : block_index;
                auto delta = root.get("delta");
                if (delta.valid() && cur_idx < blocks.size()) {
                    auto type = delta.get("type");
                    if (type.valid()) {
                        auto type_str = type.as_str();
                        std::string delta_text;
                        if (type_str == "text_delta") {
                            auto text = delta.get("text");
                            if (text.valid()) {
                                delta_text = std::string(text.as_str());
                                blocks[cur_idx].accumulated_text += delta_text;
                            }
                        } else if (type_str == "input_json_delta") {
                            auto pj = delta.get("partial_json");
                            if (pj.valid()) {
                                delta_text = std::string(pj.as_str());
                                blocks[cur_idx].accumulated_json += delta_text;
                            }
                        } else if (type_str == "thinking_delta") {
                            auto thinking = delta.get("thinking");
                            if (thinking.valid()) {
                                delta_text = std::string(thinking.as_str());
                                blocks[cur_idx].accumulated_text += delta_text;
                            }
                        }
                        if (!delta_text.empty() && options.on_event) {
                            ContentBlockDelta ev;
                            ev.index = cur_idx;
                            ev.delta_text = delta_text;
                            (*options.on_event)(ev);
                        }
                    }
                }
            } else if (event_type == "content_block_stop") {
                auto idx = root.get("index");
                auto cur_idx = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : block_index;
                // Finalize block content
                if (cur_idx < blocks.size()) {
                    auto& accum = blocks[cur_idx];
                    if (auto* tb = std::get_if<TextBlock>(&accum.block)) {
                        tb->text = std::move(accum.accumulated_text);
                    } else if (auto* tub = std::get_if<ToolUseBlock>(&accum.block)) {
                        tub->input_json = accum.accumulated_json.empty() ? "{}" : std::move(accum.accumulated_json);
                    } else if (auto* thk = std::get_if<ThinkingBlock>(&accum.block)) {
                        thk->thinking = std::move(accum.accumulated_text);
                    }
                }
                if (options.on_event) {
                    ContentBlockStop ev;
                    ev.index = cur_idx;
                    (*options.on_event)(ev);
                }
            } else if (event_type == "message_delta") {
                auto delta = root.get("delta");
                if (delta.valid()) {
                    auto sr = delta.get("stop_reason");
                    if (sr.valid() && !sr.is_null()) {
                        result.message.stop_reason = std::string(sr.as_str());
                    }
                }
                auto usage = root.get("usage");
                if (usage.valid()) {
                    auto ot = usage.get("output_tokens");
                    if (ot.valid()) result.usage.output_tokens = static_cast<std::uint32_t>(ot.as_int());
                    auto cache_creation = usage.get("cache_creation_input_tokens");
                    if (cache_creation.valid()) {
                        result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                    }
                    auto cache_read = usage.get("cache_read_input_tokens");
                    if (cache_read.valid()) {
                        result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                    }
                }
            } else if (event_type == "message_stop") {
                // Stream completed successfully
            } else if (event_type == "error") {
                auto err = root.get("error");
                if (err.valid()) {
                    std::string type_text = "stream_error";
                    std::string message_text = "Streaming API error";
                    auto type = err.get("type");
                    if (type.valid()) type_text = std::string(type.as_str());
                    auto msg = err.get("message");
                    if (msg.valid()) message_text = std::string(msg.as_str());
                    emit_stream_error(std::move(type_text), std::move(message_text));
                }
            }
        };

        // Track current SSE event type
        // (now held inside sse_decoder_)

        // Streaming POST using httplib's send() with content_receiver on Request
        httplib::Request req;
        req.method = "POST";
        req.path = endpoint->path;
        req.headers = headers;
        req.headers.emplace("Content-Type", "application/json");
        req.body = body;
        req.content_receiver = [&](const char* data, size_t len,
                                   uint64_t /*offset*/, uint64_t /*total*/) -> bool {
            if (should_abort()) return false;

            // The decoder buffers partial chunks and yields complete events;
            // dispatch each to the domain handler (parse_sse_event).
            for (const auto& ev : sse_decoder_.feed(std::string_view(data, len))) {
                parse_sse_event(ev.type, ev.data);
            }

            return true;
        };

        httplib::Response res;
        httplib::Error err;
        bool ok = cli.send(req, res, err);

        if (!ok || (res.status >= 400 && blocks.empty())) {
            // If streaming failed and we got nothing, fall back to non-streaming
            auto fallback = send_request(options);
            if (fallback) {
                result.message = std::move(fallback->message);
                result.usage = fallback->usage;
            } else {
                emit_stream_error("api_error", fallback.error().format());
                return result;
            }
        } else if (result.failed && blocks.empty()) {
            return result;
        } else {
            // Assemble final message from accumulated blocks
            result.message.timestamp = std::chrono::system_clock::now();
            for (auto& accum : blocks) {
                result.message.content.push_back(std::move(accum.block));
            }
            // Parse input_tokens from response if available
            auto usage_hdr = res.get_header_value("x-usage-input-tokens");
            if (!usage_hdr.empty()) {
                try { result.usage.input_tokens = static_cast<std::uint32_t>(std::stoul(usage_hdr)); }
                catch (...) {}
            }
        }

        // Check for tool use
        for (const auto& block : result.message.content) {
            if (std::holds_alternative<ToolUseBlock>(block)) {
                result.has_tool_use = true;
                break;
            }
        }

        // Emit stream end
        if (!result.failed && options.on_event) {
            StreamEnd end_event;
            end_event.stop_reason = result.message.stop_reason;
            end_event.usage = result.usage;
            (*options.on_event)(end_event);
        }

        return result;
    }

    /// Execute all pending tool-use blocks from an assistant message
    /// Uses parallel execution for read-only tools (P1-5)
    [[nodiscard]] std::vector<ToolResultMessage> execute_pending_tools(
        const AssistantMessage& msg,
        const QueryOptions& options) {

        // Collect all tool_use blocks
        std::vector<const ToolUseBlock*> tool_uses;
        for (const auto& block : msg.content) {
            auto* tool_use = std::get_if<ToolUseBlock>(&block);
            if (tool_use) tool_uses.push_back(tool_use);
        }

        if (tool_uses.empty()) return {};

        // Check if all tools are read-only (safe for parallel execution)
        static const std::unordered_set<std::string> readonly_tools{
            "Read", "Glob", "Grep", "LS", "FileRead"
        };

        bool all_readonly = tool_uses.size() > 1 &&
            std::ranges::all_of(tool_uses, [](const ToolUseBlock* tu) {
                return readonly_tools.contains(tu->name);
            });

        if (all_readonly) {
            // P1-5: Parallel execution for read-only tools
            std::vector<std::future<ToolResultMessage>> futures;
            futures.reserve(tool_uses.size());

            for (const auto* tool_use : tool_uses) {
                futures.push_back(std::async(std::launch::async,
                    [this, tool_use, &options]() -> ToolResultMessage {
                        return execute_single_tool(*tool_use, options);
                    }));
            }

            std::vector<ToolResultMessage> results;
            results.reserve(futures.size());
            for (auto& f : futures) {
                results.push_back(f.get());
            }
            return results;
        }

        // Sequential execution for non-readonly or single tools
        std::vector<ToolResultMessage> results;
        for (const auto* tool_use : tool_uses) {
            results.push_back(execute_single_tool(*tool_use, options));
        }
        return results;
    }

    /// Build a tool-result message for blocked tool execution.
    [[nodiscard]] ToolResultMessage make_tool_error_result(
        const ToolUseBlock& tool_use,
        std::string message) {
        ToolResultMessage result_msg{};
        result_msg.id.value = generate_id();
        result_msg.timestamp = std::chrono::system_clock::now();
        result_msg.tool_use_id = tool_use.id;
        result_msg.is_error = true;
        result_msg.content.push_back(TextBlock{std::move(message)});
        return result_msg;
    }

    struct ToolPermissionCheck {
        bool allowed = true;
        std::optional<std::string> updated_input_json;
        std::optional<std::string> message;
    };

    /// Execute a single tool and return the result message
    [[nodiscard]] ToolResultMessage execute_single_tool(
        const ToolUseBlock& tool_use,
        const QueryOptions& options) {
        if (!is_tool_enabled_for_query(tool_use.name, options)) {
            return make_tool_error_result(
                tool_use,
                std::format("Tool disabled for this query: {}", tool_use.name));
        }

        auto permission = check_tool_permission(tool_use.name, tool_use.input_json, tool_use.id.value);
        std::string effective_input_json = permission.updated_input_json.value_or(tool_use.input_json);
        if (!permission.allowed) {
            // Record denial
            {
                std::lock_guard lock(state_mutex_);
                permission_denials_.push_back(PermissionDenial{
                    tool_use.name,
                    tool_use.id.value,
                    tool_use.input_json});
            }

            return make_tool_error_result(
                tool_use,
                permission.message.value_or(std::format("Permission denied for tool: {}", tool_use.name)));
        }

        // Emit pre-tool-use hook and check for blocking hooks
        auto exec_start = std::chrono::steady_clock::now();

        // M6: emit tool-execution-start stream event for live result preview UI.
        // This lets the UI show a progress line as soon as the tool starts
        // executing, instead of waiting for the full API round-trip.
        if (options.on_event) {
            ToolExecutionStart ev;
            ev.tool_use_id = tool_use.id.value;
            ev.tool_name = tool_use.name;
            ev.input_json = effective_input_json;
            (*options.on_event)(ev);
        }

        if (lifecycle_hooks_) {
            auto block_reason = lifecycle_hooks_->check_and_emit_pre_tool_use(cc::hooks::PreToolUseEvent{
                .tool_name = tool_use.name,
                .tool_input_json = effective_input_json,
                .tool_use_id = tool_use.id.value,
                .timestamp = std::chrono::system_clock::now()
            });
            if (block_reason) {
                // Hook denied the tool execution
                {
                    std::lock_guard lock(state_mutex_);
                    permission_denials_.push_back(PermissionDenial{
                        tool_use.name,
                        tool_use.id.value,
                        effective_input_json});
                }
                return make_tool_error_result(
                    tool_use,
                    std::format("Hook denied tool execution: {}", *block_reason));
            }
        }

        // User-configured PreToolUse hooks (cc.utils.hooks_execution engine).
        // Mirrors src/services/tools/toolExecution.ts:884-946 where the TS
        // engine runs executePreToolHooks before tool execution and honors
        // BlockToolCall/AbortQuery by denying permission. Guarded so behavior
        // is unchanged when no user hooks are configured.
        if (user_hooks_configured_ && !user_hooks_.empty()) {
            namespace he = cc::utils::hooks_execution;
            he::HookExecutionContext ctx = user_hooks_ctx_template_;
            // Matcher needs ctx["tool"]["name"]; materialise an owned doc.
            std::string tool_doc = std::string("{\"name\": \"") +
                he::json_escape(tool_use.name) + "\"}";
            if (auto td = cc::utils::json::parse(tool_doc)) {
                ctx.set_context_doc("tool", std::move(*td));
            }
            // Payload mirrors TS `toolInput`/processedInput passthrough.
            if (auto pd = cc::utils::json::parse(effective_input_json)) {
                ctx.set_payload_doc(std::move(*pd));
            }

            auto [modified_payload, action] = he::run_api_query_hooks(
                user_hooks_,
                cc::utils::hooks_registry::HookEventType::PreToolUse,
                ctx,
                effective_input_json);

            using HEAction = he::HookResponseAction;
            if (action.action == HEAction::BlockToolCall ||
                action.action == HEAction::AbortQuery) {
                {
                    std::lock_guard lock(state_mutex_);
                    permission_denials_.push_back(PermissionDenial{
                        tool_use.name,
                        tool_use.id.value,
                        effective_input_json});
                }
                std::string reason = action.reason.empty()
                    ? std::string("user PreToolUse hook blocked the tool call")
                    : action.reason;
                return make_tool_error_result(tool_use, std::move(reason));
            }
            // hookUpdatedInput passthrough (toolHooks.ts:556-563): if the hook
            // produced a modified payload distinct from the input, use it.
            // `modified_payload` (the pair's first element) carries any
            // prompt-passthrough merged with the hook's modified_payload.
            if (action.action == HEAction::RetryWithModifiedPayload &&
                !modified_payload.empty() &&
                modified_payload != effective_input_json) {
                effective_input_json = modified_payload;
            }
        }

        // Execute via registry
        auto input = ToolInput::from_json(effective_input_json);
        auto exec_result = tool_registry_->execute(tool_use.name, input);

        ToolResultMessage result_msg{};
        result_msg.id.value = generate_id();
        result_msg.timestamp = std::chrono::system_clock::now();
        result_msg.tool_use_id = tool_use.id;

        if (exec_result) {
            // In-loop skill dispatch tracking: record each invoked skill so the
            // session knows which skills have been loaded (de-dup / telemetry).
            // The actual skill content load happens in execute_skill_tool; this
            // populates the previously-dead discovered_skills_ set.
            if (tool_use.name == "skill") {
                if (auto parsed = cc::utils::json::parse(effective_input_json)) {
                    auto name_val = parsed->root().get("name");
                    if (!name_val.is_str()) name_val = parsed->root().get("skill");
                    if (name_val.is_str()) {
                        std::lock_guard lock(state_mutex_);
                        discovered_skills_.insert(std::string(name_val.as_str()));
                    }
                }
            }
            auto& tr = *exec_result;
            result_msg.is_error = tr.is_error;
            for (auto& content : tr.content) {
                if (content.format && *content.format == "image" && content.media_type && content.data) {
                    // NOTE: width/height/size_bytes are not returned by the
                    // tool-use executor (it only forwards format/media_type/data).
                    // Left as std::nullopt; user-facing renderer falls back to
                    // "<no metadata>" text and an ASCII thumbnail seeded from
                    // the base64 payload.
                    ImageBlock ib;
                    ib.media_type = std::move(*content.media_type);
                    ib.data       = std::move(*content.data);
                    ib.source     = ImageBlockSource::Unknown;
                    result_msg.content.push_back(std::move(ib));
                    continue;
                }
                if (content.format && *content.format == "document" && content.media_type && content.data) {
                    result_msg.content.push_back(DocumentBlock{
                        .media_type = std::move(*content.media_type),
                        .data = std::move(*content.data),
                    });
                    continue;
                }
                // P1-11: Truncate large outputs with disk spill
                if (content.text.size() > 30000) {
                    auto truncated = content.text.substr(0, 30000);
                    truncated += "\n\n[Output truncated at 30000 chars. Full output omitted.]";
                    result_msg.content.push_back(TextBlock{std::move(truncated)});
                } else {
                    result_msg.content.push_back(TextBlock{std::move(content.text)});
                }
            }
        } else {
            result_msg.is_error = true;
            result_msg.content.push_back(TextBlock{exec_result.error().format()});
        }

        // Build the output preview once and reuse it for both the in-process
        // lifecycle event bus and the user-configured hook engine.
        std::string output_preview;
        if (!result_msg.content.empty()) {
            if (const auto* tb = std::get_if<TextBlock>(&result_msg.content[0])) {
                output_preview = tb->text.substr(0, 500);
            }
        }

        // M6: emit tool-execution-end stream event with the final result.
        // Completes the live result preview UI.
        if (options.on_event) {
            ToolExecutionEnd ev;
            ev.tool_use_id = tool_use.id.value;
            ev.result = output_preview;
            ev.is_error = result_msg.is_error;
            (*options.on_event)(ev);
        }

        // Emit post-tool-use lifecycle event
        if (lifecycle_hooks_) {
            auto exec_end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start);
            lifecycle_hooks_->emit_post_tool_use(cc::hooks::PostToolUseEvent{
                .tool_name = tool_use.name,
                .tool_use_id = tool_use.id.value,
                .is_error = result_msg.is_error,
                .output_preview = output_preview,
                .duration = duration,
                .timestamp = std::chrono::system_clock::now()
            });
        }

        // User-configured PostToolUse hooks (cc.utils.hooks_execution engine).
        // Mirrors src/services/tools/toolExecution.ts:1567-1577 where the TS
        // engine runs executePostToolHooks after the tool executes. On
        // BlockToolCall/AbortQuery the action reason is surfaced as an error
        // on the result (TS runPostToolUseHooks blockingError,
        // toolHooks.ts:105-115).
        if (user_hooks_configured_ && !user_hooks_.empty()) {
            namespace he = cc::utils::hooks_execution;
            he::HookExecutionContext ctx = user_hooks_ctx_template_;
            std::string tool_doc = std::string("{\"name\": \"") +
                he::json_escape(tool_use.name) + "\"}";
            if (auto td = cc::utils::json::parse(tool_doc)) {
                ctx.set_context_doc("tool", std::move(*td));
            }
            auto act = he::execute_post_tool_hooks(
                user_hooks_,
                ctx,
                tool_use.name,
                effective_input_json,
                output_preview);
            using HEAction = he::HookResponseAction;
            if (act.action == HEAction::BlockToolCall ||
                act.action == HEAction::AbortQuery) {
                result_msg.is_error = true;
                std::string reason = act.reason.empty()
                    ? std::string("user PostToolUse hook blocked the result")
                    : act.reason;
                result_msg.content.clear();
                result_msg.content.push_back(TextBlock{std::move(reason)});
            }
        }

        return result_msg;
    }

    /// Check if tool execution is allowed by the permission policy
    [[nodiscard]] ToolPermissionCheck check_tool_permission(std::string_view tool_name,
                                                            std::string_view input_json,
                                                            std::string_view tool_use_id = {}) {
        if (!permission_hook_) {
            // No hook configured — allow all (auto-approve mode)
            ToolPermissionCheck check{};
            check.allowed = true;
            return check;
        }

        permission_hook_->set_current_tool_use_id(tool_use_id);
        auto response = permission_hook_->can_use_response(tool_name, input_json);
        permission_hook_->clear_current_tool_use_id();
        const bool allowed = response.decision == cc::hooks::PermissionDecision::allow ||
                             response.decision == cc::hooks::PermissionDecision::allow_once;
        return ToolPermissionCheck{
            .allowed = allowed,
            .updated_input_json = std::move(response.updated_input_json),
            .message = std::move(response.message),
        };
    }

    /// Estimate total tokens currently consumed by conversation
    [[nodiscard]] std::uint32_t estimate_conversation_tokens() const noexcept {
        return estimate_conversation_tokens_locked();
    }

    [[nodiscard]] std::uint32_t estimate_conversation_tokens_locked() const noexcept {
        // Rough estimate: ~4 chars per token for English text
        std::uint32_t total_chars = 0;
        for (const auto& msg : conversation_) {
            std::visit([&total_chars](const auto& m) {
                for (const auto& block : m.content) {
                    if (const auto* text = std::get_if<TextBlock>(&block)) {
                        total_chars += static_cast<std::uint32_t>(text->text.size());
                    }
                }
            }, msg);
        }
        return total_chars / 4;
    }

    /// Add random jitter to a delay duration (±25%)
    [[nodiscard]] static std::chrono::milliseconds add_jitter(std::chrono::milliseconds base) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        auto jitter_range = base.count() / 4;
        std::uniform_int_distribution<long long> dist(-jitter_range, jitter_range);
        return std::chrono::milliseconds(base.count() + dist(rng));
    }

    /// Generate a unique identifier string (UUID-like)
    [[nodiscard]] static std::string generate_id() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<std::uint64_t> dist;
        return std::format("{:016x}{:016x}", dist(rng), dist(rng));
    }

    /// Generate a session ID
    [[nodiscard]] static std::string generate_session_id() {
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
        return std::format("session_{}_{}", ms, generate_id().substr(0, 8));
    }

    // ============================================================
    // Member variables
    // ============================================================

    [[nodiscard]] bool should_abort() const {
        return aborted_.load() || (external_abort_callback_ && external_abort_callback_());
    }

    QueryEngineConfig config_;
    ToolRegistry* tool_registry_;              // Non-owning reference to tools
    cc::hooks::ToolPermissionHook* permission_hook_ = nullptr;  // Optional permission policy
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks_ = nullptr; // Optional lifecycle hooks
    // User-configured hook path (PreToolUse/PostToolUse via the
    // cc.utils.hooks_execution engine). Mirrors src/utils/hooks.ts in TS.
    // When empty/disabled the tool loop is unchanged (parity with TS, which
    // only runs the pipeline when matching hooks exist).
    std::vector<cc::utils::hooks_registry::IndividualHookConfig> user_hooks_;
    cc::utils::hooks_execution::HookExecutionContext user_hooks_ctx_template_;
    bool user_hooks_configured_ = false;
    std::vector<Message> conversation_;        // Full conversation history
    TokenUsage cumulative_usage_;              // Session-wide token tracking
    std::atomic<bool> aborted_{false};         // Abort signal for in-flight requests
    std::function<bool()> external_abort_callback_; // Host-provided cancellation predicate
    mutable std::mutex conversation_mutex_;    // Guards conversation history
    mutable std::mutex state_mutex_;           // Guards mutable state
    std::uint32_t token_budget_ = 0;           // Token budget for auto-continuation (0 = disabled)

    // Additional state tracking
    SessionId session_id_;                     // Unique session identifier
    std::optional<std::filesystem::path> sessions_dir_;  // transcript dir (nullopt = no persistence)
    std::vector<PermissionDenial> permission_denials_;  // Record of permission denials
    std::unordered_set<std::string> discovered_skills_;  // Skills discovered this session
    std::unordered_set<std::string> loaded_nested_memory_paths_;
    std::unordered_set<std::string> content_replacement_seen_ids_;
    std::unordered_map<std::string, std::string> content_replacements_;
    std::chrono::system_clock::time_point session_start_;
    BudgetTracker budget_tracker_;
    ModelCost model_cost_;
    ApiClientConfig api_config_;
};

} // namespace cc::core
