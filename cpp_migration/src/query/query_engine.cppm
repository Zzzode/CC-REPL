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
#include <iterator>
#include <ranges>
#include <numeric>
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
#include <httplib.h>

export module cc.query.query_engine;

import cc.types.types;
import cc.tools.tool;
import cc.config.config;
import cc.utils.error;
import cc.utils.json;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;

export namespace cc::core {

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
    std::string api_version{"2023-06-01"};
    std::chrono::milliseconds timeout{120000};
    int max_retries{3};
    std::chrono::milliseconds base_retry_delay{1000};
};

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
    std::string api_key;                            // Anthropic API key
    std::optional<std::string> base_url;            // Custom API base URL
    std::optional<std::string> custom_system_prompt;// Custom system prompt
    std::optional<std::string> append_system_prompt;// Append to default system prompt
    std::optional<std::string> cwd;                 // Working directory for operations
    std::optional<double> max_budget_usd;           // Max budget in USD
    std::optional<std::uint32_t> max_turns;         // Max conversation turns
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
        append_message(Message{std::move(msg)});

        std::uint32_t round = 0;
        std::uint32_t continuation_count = 0;
        std::uint32_t max_tokens_retries = 0;
        const std::uint32_t max_rounds = options.max_tool_rounds.value_or(20);

        while (round < max_rounds && !aborted_.load() && !budget_tracker_.budget_exceeded) {
            // Stream a single API call
            auto [assistant_msg, round_usage, has_tool_use] = stream_single_api_call(options);

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

    /// Set the permission hook for tool execution policy
    void set_permission_hook(cc::hooks::ToolPermissionHook* hook) noexcept {
        permission_hook_ = hook;
    }

    /// Set the lifecycle hook registry for event notifications
    void set_lifecycle_hooks(cc::hooks::LifecycleHookRegistry* hooks) noexcept {
        lifecycle_hooks_ = hooks;
    }

    // ============================================================
    // Conversation management
    // ============================================================

    /// Get current token usage for the session
    [[nodiscard]] TokenUsage get_usage() const noexcept { return cumulative_usage_; }

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

    /// Clear conversation history (start fresh within same session)
    void clear_conversation() {
        std::lock_guard lock(conversation_mutex_);
        conversation_.clear();
        build_and_add_system_prompt();
    }

    /// Compact conversation history to fit within context window.
    /// Preserves system prompt and recent messages, summarizes middle.
    [[nodiscard]] cc::utils::VoidResult compact_conversation() {
        std::lock_guard lock(conversation_mutex_);
        if (conversation_.size() <= 4) {
            return {};  // Nothing to compact
        }

        // Keep first (system) and last N messages, drop middle
        constexpr std::size_t keep_recent = 6;
        if (conversation_.size() <= keep_recent + 1) return {};

        auto recent_start = conversation_.end() - static_cast<std::ptrdiff_t>(keep_recent);
        auto summary_text = build_compaction_summary(
            conversation_.begin() + 1,
            recent_start);

        // Replace middle section with a summary marker
        std::vector<Message> compacted;
        compacted.push_back(conversation_.front());  // System prompt

        // Insert compaction marker as user message
        UserMessage marker{};
        marker.id.value = generate_id();
        marker.timestamp = std::chrono::system_clock::now();
        marker.content.push_back(TextBlock{std::move(summary_text)});
        compacted.push_back(Message{std::move(marker)});

        // Keep recent messages
        compacted.insert(compacted.end(), recent_start, conversation_.end());

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

    /// Get session ID
    [[nodiscard]] const SessionId& session_id() const noexcept { return session_id_; }

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
            auto full_cmd = std::format("cd \"{}\" && {} 2>/dev/null", cwd, cmd);
            std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(full_cmd.c_str(), "r"), pclose);
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
    void append_message(Message msg) {
        bool should_compact = false;
        {
            std::lock_guard lock(conversation_mutex_);
            conversation_.push_back(std::move(msg));

            // Check if auto-compact needed (but don't call it while holding lock)
            if (config_.context_window.auto_compact &&
                context_utilization() > config_.context_window.compaction_threshold) {
                should_compact = true;
            }
        }
        // Lock released — safe to call compact which re-acquires
        if (should_compact) {
            (void)compact_conversation();
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

    [[nodiscard]] static std::string build_compaction_summary(
        std::vector<Message>::const_iterator first,
        std::vector<Message>::const_iterator last) {
        const auto count = static_cast<std::size_t>(std::distance(first, last));
        std::string summary = std::format(
            "[Conversation compacted: {} older messages summarized]\n"
            "Preserve these details from the compacted history:",
            count);
        constexpr std::size_t max_summary_chars = 8000;
        std::size_t index = 1;
        for (auto it = first; it != last; ++it, ++index) {
            auto line = std::format("\n- {} #{}: {}",
                role_to_string(get_role(*it)),
                index,
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

        while (round < max_rounds && !aborted_.load() && !budget_tracker_.budget_exceeded) {
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
            if (aborted_.load()) {
                return std::unexpected(Error::make(
                    ErrorCode::InternalError,
                    "Query aborted"));
            }

            auto result = send_request(options);
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
            for (const auto& msg : conversation_) {
                // Extract system prompt from first SystemMessage
                if (const auto* sys = std::get_if<SystemMessage>(&msg)) {
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
        if (config_.thinking_config.mode != ThinkingConfig::Mode::Disabled) {
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
        // Build URL
        std::string url = api_config_.base_url;
        if (!url.ends_with("/")) url += "/";
        url += "v1/messages";

        // Build headers
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("anthropic-version", api_config_.api_version);
        headers.emplace("x-api-key", api_config_.api_key);
        headers.emplace("User-Agent", "CC-REPL/1.0");

        // Build request body
        std::string body = build_request_body(options);

        // Create HTTP client
        httplib::Client cli(api_config_.base_url);
        cli.set_connection_timeout(api_config_.timeout);
        cli.set_read_timeout(api_config_.timeout);
        cli.set_write_timeout(api_config_.timeout);

        // Send request
        auto res = cli.Post("/v1/messages", headers, body, "application/json");

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
                auto compact_result = compact_conversation();
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
    };

    /// Stream a single API call with event callbacks
    [[nodiscard]] StreamCallResult stream_single_api_call(const QueryOptions& options) {
        StreamCallResult result;

        // Build headers
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("anthropic-version", api_config_.api_version);
        headers.emplace("x-api-key", api_config_.api_key);
        headers.emplace("User-Agent", "CC-REPL/1.0");

        // Build streaming request body
        std::string body = build_request_body(options, /*stream=*/true);

        // Create HTTP client
        httplib::Client cli(api_config_.base_url);
        cli.set_connection_timeout(api_config_.timeout);
        cli.set_read_timeout(api_config_.timeout);
        cli.set_write_timeout(api_config_.timeout);

        // SSE parsing state
        std::string sse_buffer;
        std::uint32_t block_index = 0;

        // Content accumulation
        struct BlockAccum {
            ContentBlock block;
            std::string accumulated_text;
            std::string accumulated_json;
        };
        std::vector<BlockAccum> blocks;

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
                }
            } else if (event_type == "message_stop") {
                // Stream completed successfully
            } else if (event_type == "error") {
                auto err = root.get("error");
                if (err.valid() && options.on_event) {
                    StreamError ev;
                    auto type = err.get("type");
                    if (type.valid()) ev.error_type = std::string(type.as_str());
                    auto msg = err.get("message");
                    if (msg.valid()) ev.message = std::string(msg.as_str());
                    (*options.on_event)(ev);
                }
            }
        };

        // Track current SSE event type
        std::string current_event_type;

        // Streaming POST using httplib's send() with content_receiver on Request
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/messages";
        req.headers = headers;
        req.headers.emplace("Content-Type", "application/json");
        req.body = body;
        req.content_receiver = [&](const char* data, size_t len,
                                   uint64_t /*offset*/, uint64_t /*total*/) -> bool {
            if (aborted_.load()) return false;

            sse_buffer.append(data, len);

            // Parse SSE protocol: lines ending with \n\n
            while (true) {
                auto double_nl = sse_buffer.find("\n\n");
                if (double_nl == std::string::npos) break;

                std::string event_block = sse_buffer.substr(0, double_nl);
                sse_buffer.erase(0, double_nl + 2);

                // Parse event block lines
                std::string event_data;
                std::string event_type_local;
                size_t pos = 0;
                while (pos < event_block.size()) {
                    auto nl = event_block.find('\n', pos);
                    std::string line;
                    if (nl == std::string::npos) {
                        line = event_block.substr(pos);
                        pos = event_block.size();
                    } else {
                        line = event_block.substr(pos, nl - pos);
                        pos = nl + 1;
                    }

                    if (line.starts_with("event: ")) {
                        event_type_local = line.substr(7);
                        current_event_type = event_type_local;
                    } else if (line.starts_with("data: ")) {
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(6);
                    } else if (line == "data:") {
                        if (!event_data.empty()) event_data += "\n";
                    }
                }

                if (!event_data.empty()) {
                    parse_sse_event(current_event_type, event_data);
                }
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
            }
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
        if (options.on_event) {
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

    /// Execute a single tool and return the result message
    [[nodiscard]] ToolResultMessage execute_single_tool(
        const ToolUseBlock& tool_use,
        const QueryOptions& options) {
        if (!is_tool_enabled_for_query(tool_use.name, options)) {
            return make_tool_error_result(
                tool_use,
                std::format("Tool disabled for this query: {}", tool_use.name));
        }

        // Check permission first
        bool allowed = check_tool_permission(tool_use.name, tool_use.input_json);
        if (!allowed) {
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
                std::format("Permission denied for tool: {}", tool_use.name));
        }

        // Emit pre-tool-use hook
        auto exec_start = std::chrono::steady_clock::now();
        if (lifecycle_hooks_) {
            lifecycle_hooks_->emit_pre_tool_use(cc::hooks::PreToolUseEvent{
                .tool_name = tool_use.name,
                .tool_input_json = tool_use.input_json,
                .tool_use_id = tool_use.id.value,
                .timestamp = std::chrono::system_clock::now()
            });
        }

        // Execute via registry
        auto input = ToolInput::from_json(tool_use.input_json);
        auto exec_result = tool_registry_->execute(tool_use.name, input);

        ToolResultMessage result_msg{};
        result_msg.id.value = generate_id();
        result_msg.timestamp = std::chrono::system_clock::now();
        result_msg.tool_use_id = tool_use.id;

        if (exec_result) {
            auto& tr = *exec_result;
            result_msg.is_error = tr.is_error;
            for (auto& content : tr.content) {
                if (content.format && *content.format == "image" && content.media_type && content.data) {
                    result_msg.content.push_back(ImageBlock{
                        .media_type = std::move(*content.media_type),
                        .data = std::move(*content.data),
                    });
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

        // Emit post-tool-use hook
        if (lifecycle_hooks_) {
            auto exec_end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start);
            std::string output_preview;
            if (!result_msg.content.empty()) {
                if (const auto* tb = std::get_if<TextBlock>(&result_msg.content[0])) {
                    output_preview = tb->text.substr(0, 500);
                }
            }
            lifecycle_hooks_->emit_post_tool_use(cc::hooks::PostToolUseEvent{
                .tool_name = tool_use.name,
                .tool_use_id = tool_use.id.value,
                .is_error = result_msg.is_error,
                .output_preview = std::move(output_preview),
                .duration = duration,
                .timestamp = std::chrono::system_clock::now()
            });
        }

        return result_msg;
    }

    /// Check if tool execution is allowed by the permission policy
    [[nodiscard]] bool check_tool_permission(std::string_view tool_name,
                                               std::string_view input_json) {
        if (!permission_hook_) {
            // No hook configured — allow all (auto-approve mode)
            return true;
        }

        auto decision = permission_hook_->can_use(tool_name, input_json);
        return decision == cc::hooks::PermissionDecision::allow ||
               decision == cc::hooks::PermissionDecision::allow_once;
    }

    /// Estimate total tokens currently consumed by conversation
    [[nodiscard]] std::uint32_t estimate_conversation_tokens() const noexcept {
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

    QueryEngineConfig config_;
    ToolRegistry* tool_registry_;              // Non-owning reference to tools
    cc::hooks::ToolPermissionHook* permission_hook_ = nullptr;  // Optional permission policy
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks_ = nullptr; // Optional lifecycle hooks
    std::vector<Message> conversation_;        // Full conversation history
    TokenUsage cumulative_usage_;              // Session-wide token tracking
    std::atomic<bool> aborted_{false};         // Abort signal for in-flight requests
    mutable std::mutex conversation_mutex_;    // Guards conversation history
    mutable std::mutex state_mutex_;           // Guards mutable state
    std::uint32_t token_budget_ = 0;           // Token budget for auto-continuation (0 = disabled)

    // Additional state tracking
    SessionId session_id_;                     // Unique session identifier
    std::vector<PermissionDenial> permission_denials_;  // Record of permission denials
    std::unordered_set<std::string> discovered_skills_;  // Skills discovered this session
    std::unordered_set<std::string> loaded_nested_memory_paths_;
    bool has_handled_orphaned_permission_ = false;
    std::chrono::system_clock::time_point session_start_;
    BudgetTracker budget_tracker_;
    ModelCost model_cost_;
    ApiClientConfig api_config_;
};

} // namespace cc::core
