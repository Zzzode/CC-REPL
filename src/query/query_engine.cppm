/// @file query_engine.cppm
/// @brief Query engine module - the core LLM interaction loop.
/// Manages conversation state, streaming API calls, tool execution loops,
/// retry logic, and token budget management using C++23 coroutines.
///
/// RFC 0001 Phase C batch 5: all non-trivial member bodies live in the
/// query_engine_*.cpp module implementation units. This interface keeps
/// only the complete configuration/state type definitions, the nested
/// result structs, and the trivial inline accessors. The textual
/// <httplib.h> global-module-fragment include moved to query_engine_http.cpp
/// (the only TU that names raw httplib types).
module;

export module cc.query.query_engine;

import std;

import cc.types.types;
import cc.tools.tool;
import cc.utils.error;
import cc.utils.json;
import cc.query.wire_protocol;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;
import cc.utils.hooks_registry;
import cc.utils.hooks_execution;
import cc.services.compact.api_microcompact;

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
    [[nodiscard]] std::vector<SseEvent> feed(std::string_view chunk);

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

[[nodiscard]] std::expected<ApiMessagesEndpoint, std::string> api_messages_endpoint(
    std::string_view base_url
);

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
    /// Fixed session id instead of a generated one — lets a resumed run use
    /// the same session-memory/summary.md and transcript directory.
    std::optional<std::string> session_id_override;
    /// Which wire protocol to speak. Unset => Anthropic (the historical
    /// default, so existing configs keep working unchanged). Set to
    /// "openai"/"openai-compatible" to drive any OpenAI-compatible endpoint.
    std::optional<std::string> wire_api;
    /// Whether to emit the vendor-native computer-use tool shape. Defaults to
    /// true for the Anthropic wire (its native tool is what makes the
    /// see→act loop work best there); the OpenAI wire always uses the plain
    /// function form regardless.
    std::optional<bool> native_computer_tool;
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
    /// Optional callback that provides dynamically-discovered tools
    /// (e.g. MCP server tools) to be merged with config_.tools when
    /// building the API request body.  Called on every request so
    /// newly-connected MCP servers' tools are picked up immediately.
    /// TS PARITY: assembleToolPool() merges built-in + mcp.tools; this
    /// callback is the CPP equivalent of the dynamic MCP portion.
    std::function<std::vector<ToolDefinition>()> dynamic_tools_provider;
    /// Optional callback snapshotting connected MCP tools' verbatim input
    /// schemas as {tool_name -> schema JSON}. Invoked at most once per
    /// request. MCP-merged tool defs carry an empty simplified schema
    /// (nested shapes cannot be represented there); this hook lets the
    /// request serializer emit the servers' real schemas. Injected from the
    /// MCP wiring layer so this module does not depend on the (large) MCP
    /// module.
    std::function<std::unordered_map<std::string, std::string>()>
        mcp_input_schema_provider;
    /// Flat list of raw permission deny rules (e.g. "Bash",
    /// "mcp__linear", "mcp__linear__*", "Bash(npm install)") applied to
    /// BOTH static and dynamic tools before the request "tools" array is
    /// serialized.
    // TS PARITY: context.alwaysDenyRules flattened over all sources
    // (user/project/local/flag/policy/cliArg/command/session — see
    // permissions.ts:109-114,213-221); matched via
    // cc::utils::tool_deny_rules before the tools array is serialized.
    std::vector<std::string> always_deny_rules;
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
        const SystemContext& system_ctx);

private:
    /// Build default system prompt
    [[nodiscard]] static std::string build_default_system_prompt();
};

// ============================================================
// Query Engine - main LLM interaction engine
// ============================================================

/// Core engine orchestrating LLM API interactions, tool execution,
/// and conversation state management.
class QueryEngine {
public:
    /// Construct engine with configuration and tool registry
    explicit QueryEngine(QueryEngineConfig config, ToolRegistry& registry);

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
        const QueryOptions& options = {});

    /// Stream a query with real-time event callbacks
    void stream_query(
        std::string_view user_message,
        const QueryOptions& options = {});

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
    [[nodiscard]] double context_utilization() const noexcept;

    /// Get the full conversation history (thread-safe copy)
    [[nodiscard]] std::vector<Message> get_conversation() const;

    /// Restore conversation history for resumed sessions and rebuild content replacement state.
    void restore_conversation(std::vector<Message> messages);

    /// Clear conversation history (start fresh within same session)
    void clear_conversation();

    /// Compact conversation history to fit within context window.
    /// Preserves system prompt and recent messages, summarizes middle.
    [[nodiscard]] cc::utils::VoidResult compact_conversation(std::string_view trigger = "manual");

    void append_message_for_testing(Message msg) {
        append_message(std::move(msg));
    }

    /// Update model parameters at runtime
    void set_model_params(ModelParams params) { config_.model_params = std::move(params); }

    /// Get current model parameters
    [[nodiscard]] const ModelParams& model_params() const noexcept { return config_.model_params; }

    /// Get the effective working directory used by the engine.
    [[nodiscard]] std::string working_directory() const;

    /// Update the engine's working directory (e.g. after a user !cd command).
    void set_working_directory(std::string path) {
        config_.cwd = std::move(path);
    }

    /// Get session ID
    [[nodiscard]] const SessionId& session_id() const noexcept { return session_id_; }

    /// Skills invoked via the skill tool during this session (in-loop dispatch
    /// tracking). Populated by execute_single_tool on each successful skill call.
    [[nodiscard]] std::vector<std::string> discovered_skills() const;

    /// Testing seam: invoke a single tool-use block through the same path as
    /// the live tool loop (so in-loop tracking like discovered_skills_ fires).
    [[nodiscard]] ToolResultMessage execute_single_tool_for_testing(
        const ToolUseBlock& tool_use) {
        QueryOptions options;
        return execute_single_tool(tool_use, options);
    }

    /// Get permission denials
    [[nodiscard]] std::vector<PermissionDenial> get_permission_denials() const;

    /// Get current budget status
    [[nodiscard]] const BudgetTracker& budget_tracker() const noexcept { return budget_tracker_; }

private:
    // ============================================================
    // Internal implementation
    // ============================================================

    /// Setup API client configuration
    void setup_api_client();

    /// Build and add system prompt to conversation
    void build_and_add_system_prompt();

    /// Populate user context with git branch and last commit info
    static void populate_git_context(UserContext& ctx, const std::string& cwd);

    // Flatten recent user/assistant turns to plain text for the extractor.
    [[nodiscard]] static std::string transcript_to_text(
        const std::vector<Message>& messages, std::size_t max_chars);

    // Kick off a detached LLM extraction turn when thresholds are met.
    // Mirrors TS extractMemories (throttle by new-message count, single
    // in-flight extraction, sub-agent with file tools writes the memories).
    void maybe_run_memory_extraction();

    /// Build a UserMessage from raw text input
    [[nodiscard]] UserMessage make_user_message(std::string_view text,
                                                 std::optional<std::string_view> uuid = std::nullopt) const;

    /// Thread-safe append to conversation history
public:
    /// Enable transcript persistence. After this call, every appended message
    /// is also appended (JSONL) to <sessions_dir>/<session_id>/messages.jsonl,
    /// and session metadata is written so the session is discoverable by
    /// cc::session::list_recent_sessions. Call once at startup.
    void set_session_storage(std::filesystem::path sessions_dir);

    // TS REF: src/services/api/dumpPrompts.ts
    // Enable full API request/response dump.  Every API call writes:
    //   {"type":"request","timestamp":...,"body":<full request JSON>}
    //   {"type":"response","timestamp":...,"events":[<parsed SSE events>]}
    // to <dump_dir>/<session_id>.jsonl.
    void set_dump_prompts_dir(std::filesystem::path dump_dir);

    [[nodiscard]] std::optional<std::filesystem::path> dump_prompts_path() const {
        if (!dump_prompts_dir_) return std::nullopt;
        return *dump_prompts_dir_ / (session_id_.str() + ".jsonl");
    }

    /// Refresh session metadata on disk. Message bodies are appended
    /// incrementally in append_message(), so this only rewrites metadata.json
    /// with the current message count and last-active timestamp.
    void flush_session();

private:
    /// Serialize a single Message to a one-line JSON string for the transcript.
    /// Reuses the API-request serializer; SystemMessage has no on-wire form and
    /// serializes to empty (skipped by append_message).
    [[nodiscard]] std::string message_to_jsonl_(const Message& msg) const;

    void append_message(Message msg);

    void append_pending_native_agent_notifications();

    struct QueryToolResultBudgetCandidate {
        std::size_t message_index = 0;
        std::string tool_use_id;
        std::string tool_name;
        std::size_t size = 0;
    };

    [[nodiscard]] static std::string lowercase_ascii(std::string_view value);

    [[nodiscard]] static bool tool_result_already_replaced(std::string_view text);

    [[nodiscard]] static bool env_truthy_any(std::initializer_list<const char*> names);

    [[nodiscard]] static std::uint32_t env_uint_or_default(
        std::initializer_list<const char*> names,
        std::uint32_t fallback);

    [[nodiscard]] static bool time_based_microcompact_enabled();

    [[nodiscard]] static std::uint32_t time_based_microcompact_gap_minutes();

    [[nodiscard]] static std::uint32_t time_based_microcompact_keep_recent();

    [[nodiscard]] static bool is_time_based_microcompact_tool(std::string_view tool_name);

    [[nodiscard]] std::unordered_set<std::string> unbounded_tool_result_budget_names() const;

    [[nodiscard]] static std::unordered_map<std::string, std::string> tool_name_by_tool_use_id(
        const std::vector<Message>& messages);

    [[nodiscard]] static std::optional<std::string> tool_result_plain_text_content(
        const ToolResultMessage& message);

    [[nodiscard]] static std::optional<std::string> tool_result_text_for_budget(
        const ToolResultMessage& message);

    [[nodiscard]] static std::vector<std::vector<QueryToolResultBudgetCandidate>>
    collect_tool_result_budget_candidates_by_message(
        const std::vector<Message>& messages,
        const std::unordered_map<std::string, std::string>& tool_names);

    [[nodiscard]] static std::optional<std::chrono::system_clock::time_point>
    last_assistant_timestamp(const std::vector<Message>& messages);

    void apply_time_based_microcompact();

    [[nodiscard]] std::filesystem::path query_tool_result_path(std::string_view tool_use_id) const;

    [[nodiscard]] std::optional<std::string> build_query_tool_result_replacement(
        const QueryToolResultBudgetCandidate& candidate,
        std::string_view content) const;

    void replace_tool_result_message_locked(std::size_t message_index, std::string replacement);

    void rebuild_content_replacement_state_locked();

    void apply_tool_result_budget();

    [[nodiscard]] static std::string truncate_for_compaction(std::string text,
                                                             std::size_t limit);

    [[nodiscard]] static std::string content_block_compaction_text(const ContentBlock& block);

    [[nodiscard]] static std::string message_compaction_text(const Message& message);

    [[nodiscard]] static bool is_compact_boundary_message(const Message& message);

    [[nodiscard]] static bool is_snip_boundary_message(const Message& message);

    void replay_snip_boundaries();

    [[nodiscard]] static std::string message_id_value(const Message& message);

    [[nodiscard]] static std::optional<CompactPreservedSegment> compact_preserved_segment(
        std::vector<Message>::const_iterator first,
        std::vector<Message>::const_iterator last,
        std::string_view anchor_id);

    /// Read the session's accumulated compaction summary.md (empty when
    /// absent/unreadable). TS REF: getSessionMemoryContent.
    [[nodiscard]] std::string read_session_summary(std::string_view cwd) const;

    /// Append one compaction summary to the session's summary.md, creating
    /// the session-memory directory as needed. Best-effort: a write failure
    /// must not break an in-memory compaction that already succeeded.
    void append_session_summary(std::string_view cwd,
                                std::string_view summary) const;

    [[nodiscard]] static std::string build_compaction_summary(
        std::vector<Message>::const_iterator first,
        std::vector<Message>::const_iterator last);

    /// Tool loop result structure
    struct ToolLoopResult {
        AssistantMessage message;
        TokenUsage usage;
        std::uint32_t rounds;
    };

    /// Execute the full tool-call loop until completion or abort
    [[nodiscard]] Result<ToolLoopResult> execute_tool_loop(const QueryOptions& options);

    /// Single API call result
    struct ApiCallResult {
        AssistantMessage message;
        TokenUsage usage;
    };

    /// Execute a single API call with retry logic
    [[nodiscard]] Result<ApiCallResult> call_api(const QueryOptions& options);

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
        const QueryOptions& options);

    [[nodiscard]] std::optional<cc::services::compact::ContextManagementConfig>
    api_context_management() const;

    [[nodiscard]] bool thinking_enabled_for_request() const;

    void add_output_config_to_json(
        cc::utils::json::JsonMutVal& root,
        cc::utils::json::JsonMutDoc& doc
    ) const;

public:
    /// Build the output_config JSON fragment (for testing / introspection).
    /// Returns "{}" when neither task_budget nor response_schema is configured.
    [[nodiscard]] std::string build_output_config_json_for_testing() const;

    /// Build a non-streaming API request body (for testing / introspection).
    /// Runs the full tool-merge path including always_deny_rules filtering.
    [[nodiscard]] std::string build_request_body_for_testing(
        const QueryOptions& options = {}) const {
        return build_request_body(options);
    }

private:

    void update_task_budget_remaining_after_compact(std::uint32_t pre_compact_tokens);

    void restore_task_budget_remaining_from_compact_boundaries_locked();

    /// Construct the wire backend selected by `wire_api_`. Called per request
    /// (cheap) so a config change cannot leave a stale backend behind.
    [[nodiscard]] std::unique_ptr<cc::query::wire::WireBackend>
    make_wire_backend() const;

    /// Collect everything a backend needs for one request. This is where the
    /// engine's conversation walk (system-prompt hoisting, snip filtering,
    /// compact-boundary skipping) and tool pruning live — the backends stay
    /// ignorant of the engine's message model.
    [[nodiscard]] cc::query::wire::RequestInput build_wire_input(
        const QueryOptions& options,
        bool stream) const;

    /// Build HTTP request body
    [[nodiscard]] std::string build_request_body(
        const QueryOptions& options,
        bool stream = false) const;


    /// Append a Message variant to JSON array
    void append_message_to_json(const Message& msg,
                                cc::utils::json::JsonMutVal& arr,
                                cc::utils::json::JsonMutDoc& doc) const;

    /// Convert content blocks to JSON
    [[nodiscard]] cc::utils::json::JsonMutVal content_to_json(
        const std::vector<ContentBlock>& content,
        cc::utils::json::JsonMutDoc& doc) const;

    /// Low-level API request using httplib
    [[nodiscard]] Result<ApiCallResult> send_request(
        const QueryOptions& options,
        bool is_retry_after_compact = false);

    /// Classify HTTP status code to error code
    [[nodiscard]] ErrorCode classify_http_error(int status) const;

    /// Parse API response to our types
    [[nodiscard]] Result<ApiCallResult> parse_api_response(std::string_view response_body);

    /// Parse a single content block from JSON
    void parse_content_block(cc::utils::json::JsonVal block,
                            std::vector<ContentBlock>& content) const;

    /// Single streaming API call result
    struct StreamCallResult {
        AssistantMessage message;
        TokenUsage usage;
        bool has_tool_use = false;
        bool failed = false;
        std::string error_message;
    };

    /// Stream a single API call with event callbacks
    [[nodiscard]] StreamCallResult stream_single_api_call(const QueryOptions& options);

    /// Execute all pending tool-use blocks from an assistant message
    /// Uses parallel execution for read-only tools (P1-5)
    [[nodiscard]] std::vector<ToolResultMessage> execute_pending_tools(
        const AssistantMessage& msg,
        const QueryOptions& options);

    /// Build a tool-result message for blocked tool execution.
    [[nodiscard]] ToolResultMessage make_tool_error_result(
        const ToolUseBlock& tool_use,
        std::string message);

    struct ToolPermissionCheck {
        bool allowed = true;
        std::optional<std::string> updated_input_json;
        std::optional<std::string> message;
    };

    /// Execute a single tool and return the result message
    [[nodiscard]] ToolResultMessage execute_single_tool(
        const ToolUseBlock& tool_use,
        const QueryOptions& options);

    /// Check if tool execution is allowed by the permission policy
    [[nodiscard]] ToolPermissionCheck check_tool_permission(std::string_view tool_name,
                                                            std::string_view input_json,
                                                            std::string_view tool_use_id = {});

    /// Estimate total tokens currently consumed by conversation
    [[nodiscard]] std::uint32_t estimate_conversation_tokens() const noexcept {
        return estimate_conversation_tokens_locked();
    }

    [[nodiscard]] std::uint32_t estimate_conversation_tokens_locked() const noexcept;

    /// Add random jitter to a delay duration (±25%)
    [[nodiscard]] static std::chrono::milliseconds add_jitter(std::chrono::milliseconds base);

    /// Generate a unique identifier string (UUID-like)
    [[nodiscard]] static std::string generate_id();

    /// Generate a session ID
    [[nodiscard]] static std::string generate_session_id();

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
    std::optional<std::filesystem::path> dump_prompts_dir_;  // API req/resp dump dir
    std::vector<PermissionDenial> permission_denials_;  // Record of permission denials
    std::unordered_set<std::string> discovered_skills_;  // Skills discovered this session
    std::unordered_set<std::string> loaded_nested_memory_paths_;
    std::unordered_set<std::string> content_replacement_seen_ids_;
    std::unordered_map<std::string, std::string> content_replacements_;
    std::chrono::system_clock::time_point session_start_;
    BudgetTracker budget_tracker_;
    ModelCost model_cost_;
    ApiClientConfig api_config_;
    /// Selected wire protocol. Set in setup_api_client(); read when
    /// serializing requests and parsing responses.
    cc::query::wire::WireApi wire_api_ = cc::query::wire::WireApi::Anthropic;
    /// When true, emit the vendor-native computer-use tool shape.
    bool native_computer_tool_ = true;

    // ── Post-turn LLM memory extraction (TS extractMemories) ────────────────
    // After enough NEW messages accumulate, a background sub-agent reads the
    // recent transcript and writes durable frontmatter memories into the
    // auto-memory dir. Runs detached so it never blocks the interactive loop;
    // failures are logged, never surfaced. Only one extraction runs at a time.
    // New-message throttle for post-turn LLM memory extraction.
    std::size_t messages_since_last_extraction_ = 0;
    // Owned extraction thread: joined in the destructor, so the captured
    // ToolRegistry and `this`-adjacent state always outlive the sub-agent.
    std::atomic<bool> memory_extraction_inflight_{false};
    std::jthread memory_extraction_thread_;
    // Set false on the extraction sub-engine so it does not recursively spawn
    // its own extractor after its (internal) query() turn.
    bool memory_extraction_enabled_ =
        [] {
            // TS gates auto-extraction behind a feature flag; default OFF so
            // headless/server/test runs don't fire background sub-agent API
            // calls. Opt in with LOOM_ENABLE_MEMORY_EXTRACTION=1.
            const char* v = std::getenv("LOOM_ENABLE_MEMORY_EXTRACTION");
            return v && (std::string_view(v) == "1" ||
                         std::string_view(v) == "true");
        }();
};

} // namespace cc::core
