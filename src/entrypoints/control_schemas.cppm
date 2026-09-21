/// @file control_schemas.cppm
/// @brief SDK Control Schemas - struct definitions with validation for the control protocol.
/// Migrated from src/entrypoints/sdk/controlSchemas.ts
///
/// These schemas define the control protocol between SDK implementations and the CLI.
/// Used by SDK builders (e.g., Python SDK) to communicate with the CLI process.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <cstdint>

export module cc.entrypoints.control_schemas;

import cc.entrypoints.core_schemas;

export namespace cc::entrypoints::control {

// ============================================================================
// Hook Callback Types
// ============================================================================

/// Configuration for matching and routing hook callbacks
struct HookCallbackMatcher {
    std::optional<std::string> matcher;
    std::vector<std::string> hook_callback_ids;
    std::optional<double> timeout;
};

// ============================================================================
// Control Request Types
// ============================================================================

/// Initializes the SDK session with hooks, MCP servers, and agent configuration
struct ControlInitializeRequest {
    static constexpr auto subtype = "initialize";
    std::optional<std::unordered_map<std::string, std::vector<HookCallbackMatcher>>> hooks;
    std::optional<std::vector<std::string>> sdk_mcp_servers;
    std::optional<std::unordered_map<std::string, std::string>> json_schema;
    std::optional<std::string> system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::unordered_map<std::string, core_schemas::AgentDefinition>> agents;
    std::optional<bool> prompt_suggestions;
    std::optional<bool> agent_progress_summaries;
};

/// Response from session initialization
struct ControlInitializeResponse {
    std::vector<core_schemas::SlashCommand> commands;
    std::vector<core_schemas::AgentInfo> agents;
    std::string output_style;
    std::vector<std::string> available_output_styles;
    std::vector<core_schemas::ModelInfo> models;
    core_schemas::AccountInfo account;
    std::optional<int> pid;  // CLI process PID for tmux socket isolation
    std::optional<core_schemas::FastModeState> fast_mode_state;
};

/// Interrupts the currently running conversation turn
struct ControlInterruptRequest {
    static constexpr auto subtype = "interrupt";
};

/// Requests permission to use a tool with the given input
struct ControlPermissionRequest {
    static constexpr auto subtype = "can_use_tool";
    std::string tool_name;
    std::unordered_map<std::string, std::string> input;
    std::optional<std::vector<core_schemas::PermissionUpdate>> permission_suggestions;
    std::optional<std::string> blocked_path;
    std::optional<std::string> decision_reason;
    std::optional<std::string> title;
    std::optional<std::string> display_name;
    std::string tool_use_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> description;
};

/// Sets the permission mode for tool execution handling
struct ControlSetPermissionModeRequest {
    static constexpr auto subtype = "set_permission_mode";
    core_schemas::PermissionMode mode;
    std::optional<bool> ultraplan;  // @internal CCR ultraplan session marker
};

/// Sets the model to use for subsequent conversation turns
struct ControlSetModelRequest {
    static constexpr auto subtype = "set_model";
    std::optional<std::string> model;
};

/// Sets the maximum number of thinking tokens for extended thinking
struct ControlSetMaxThinkingTokensRequest {
    static constexpr auto subtype = "set_max_thinking_tokens";
    std::optional<int> max_thinking_tokens;  // nullable represented as optional
};

/// Requests the current status of all MCP server connections
struct ControlMcpStatusRequest {
    static constexpr auto subtype = "mcp_status";
};

/// Response containing MCP server connection status
struct ControlMcpStatusResponse {
    std::vector<core_schemas::McpServerStatus> mcp_servers;
};

/// Requests a breakdown of current context window usage by category
struct ControlGetContextUsageRequest {
    static constexpr auto subtype = "get_context_usage";
};

/// Context category information
struct ContextCategory {
    std::string name;
    int tokens = 0;
    std::string color;
    std::optional<bool> is_deferred;
};

/// Context grid square for visualization
struct ContextGridSquare {
    std::string color;
    bool is_filled = false;
    std::string category_name;
    int tokens = 0;
    double percentage = 0.0;
    double square_fullness = 0.0;
};

/// Memory file info
struct MemoryFileInfo {
    std::string path;
    std::string type;
    int tokens = 0;
};

/// MCP tool info in context usage
struct McpToolInfo {
    std::string name;
    std::string server_name;
    int tokens = 0;
    std::optional<bool> is_loaded;
};

/// Deferred builtin tool info
struct DeferredBuiltinToolInfo {
    std::string name;
    int tokens = 0;
    bool is_loaded = false;
};

/// System tool info
struct SystemToolInfo {
    std::string name;
    int tokens = 0;
};

/// System prompt section
struct SystemPromptSection {
    std::string name;
    int tokens = 0;
};

/// Agent context usage info
struct AgentContextInfo {
    std::string agent_type;
    std::string source;
    int tokens = 0;
};

/// Slash commands context info
struct SlashCommandsContextInfo {
    int total_commands = 0;
    int included_commands = 0;
    int tokens = 0;
};

/// Skill frontmatter info
struct SkillFrontmatterInfo {
    std::string name;
    std::string source;
    int tokens = 0;
};

/// Skills context info
struct SkillsContextInfo {
    int total_skills = 0;
    int included_skills = 0;
    int tokens = 0;
    std::vector<SkillFrontmatterInfo> skill_frontmatter;
};

/// Tool usage by type
struct ToolCallsByTypeInfo {
    std::string name;
    int call_tokens = 0;
    int result_tokens = 0;
};

/// Attachment by type
struct AttachmentsByTypeInfo {
    std::string name;
    int tokens = 0;
};

/// Message breakdown in context usage
struct MessageBreakdown {
    int tool_call_tokens = 0;
    int tool_result_tokens = 0;
    int attachment_tokens = 0;
    int assistant_message_tokens = 0;
    int user_message_tokens = 0;
    std::vector<ToolCallsByTypeInfo> tool_calls_by_type;
    std::vector<AttachmentsByTypeInfo> attachments_by_type;
};

/// API usage stats
struct ApiUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_creation_input_tokens = 0;
    int cache_read_input_tokens = 0;
};

/// Full context usage response
struct ControlGetContextUsageResponse {
    std::vector<ContextCategory> categories;
    int total_tokens = 0;
    int max_tokens = 0;
    int raw_max_tokens = 0;
    double percentage = 0.0;
    std::vector<std::vector<ContextGridSquare>> grid_rows;
    std::string model;
    std::vector<MemoryFileInfo> memory_files;
    std::vector<McpToolInfo> mcp_tools;
    std::optional<std::vector<DeferredBuiltinToolInfo>> deferred_builtin_tools;
    std::optional<std::vector<SystemToolInfo>> system_tools;
    std::optional<std::vector<SystemPromptSection>> system_prompt_sections;
    std::vector<AgentContextInfo> agents;
    std::optional<SlashCommandsContextInfo> slash_commands;
    std::optional<SkillsContextInfo> skills;
    std::optional<int> auto_compact_threshold;
    bool is_auto_compact_enabled = false;
    std::optional<MessageBreakdown> message_breakdown;
    std::optional<ApiUsage> api_usage;  // nullable in TS
};

/// Rewinds file changes made since a specific user message
struct ControlRewindFilesRequest {
    static constexpr auto subtype = "rewind_files";
    std::string user_message_id;
    std::optional<bool> dry_run;
};

/// Result of a rewindFiles operation
struct ControlRewindFilesResponse {
    bool can_rewind = false;
    std::optional<std::string> error;
    std::optional<std::vector<std::string>> files_changed;
    std::optional<int> insertions;
    std::optional<int> deletions;
};

/// Drops a pending async user message from the command queue
struct ControlCancelAsyncMessageRequest {
    static constexpr auto subtype = "cancel_async_message";
    std::string message_uuid;
};

/// Result of cancel_async_message operation
struct ControlCancelAsyncMessageResponse {
    bool cancelled = false;
};

/// Seeds the readFileState cache with a path+mtime entry
struct ControlSeedReadStateRequest {
    static constexpr auto subtype = "seed_read_state";
    std::string path;
    double mtime = 0;
};

/// Delivers a hook callback with its input data
struct HookCallbackRequest {
    static constexpr auto subtype = "hook_callback";
    std::string callback_id;
    std::string tool_use_id;
    // input is a complex union type, simplified as a generic map
    std::unordered_map<std::string, std::string> input;
};

/// Sends a JSON-RPC message to a specific MCP server
struct ControlMcpMessageRequest {
    static constexpr auto subtype = "mcp_message";
    std::string server_name;
    std::string message;  // JSON-RPC message as serialized string
};

/// Replaces the set of dynamically managed MCP servers
struct ControlMcpSetServersRequest {
    static constexpr auto subtype = "mcp_set_servers";
    std::unordered_map<std::string, core_schemas::McpServerConfig> servers;
};

/// Result of replacing MCP servers
struct ControlMcpSetServersResponse {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::unordered_map<std::string, std::string> errors;
};

/// Reloads plugins from disk
struct ControlReloadPluginsRequest {
    static constexpr auto subtype = "reload_plugins";
};

/// Plugin info
struct PluginInfo {
    std::string name;
    std::string path;
    std::optional<std::string> source;
};

/// Response after plugin reload
struct ControlReloadPluginsResponse {
    std::vector<core_schemas::SlashCommand> commands;
    std::vector<core_schemas::AgentInfo> agents;
    std::vector<PluginInfo> plugins;
    std::vector<core_schemas::McpServerStatus> mcp_servers;
    int error_count = 0;
};

/// Reconnects a disconnected or failed MCP server
struct ControlMcpReconnectRequest {
    static constexpr auto subtype = "mcp_reconnect";
    std::string server_name;
};

/// Enables or disables an MCP server
struct ControlMcpToggleRequest {
    static constexpr auto subtype = "mcp_toggle";
    std::string server_name;
    bool enabled = false;
};

/// Stops a running task
struct ControlStopTaskRequest {
    static constexpr auto subtype = "stop_task";
    std::string task_id;
};

/// Merges settings into the flag settings layer
struct ControlApplyFlagSettingsRequest {
    static constexpr auto subtype = "apply_flag_settings";
    std::unordered_map<std::string, std::string> settings;
};

/// Returns the effective merged settings
struct ControlGetSettingsRequest {
    static constexpr auto subtype = "get_settings";
};

/// Settings source entry
enum class SettingsSourceName : std::uint8_t {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    FlagSettings,
    PolicySettings,
};

/// A single source entry in settings
struct SettingsSourceEntry {
    SettingsSourceName source;
    std::unordered_map<std::string, std::string> settings;
};

/// Effort level for applied settings
enum class EffortLevel : std::uint8_t {
    Low,
    Medium,
    High,
    Max,
};

/// Applied runtime-resolved values
struct AppliedSettings {
    std::string model;
    std::optional<EffortLevel> effort;  // nullable
};

/// Response with effective merged settings
struct ControlGetSettingsResponse {
    std::unordered_map<std::string, std::string> effective;
    std::vector<SettingsSourceEntry> sources;
    std::optional<AppliedSettings> applied;
};

/// Requests the SDK consumer to handle an MCP elicitation
struct ControlElicitationRequest {
    static constexpr auto subtype = "elicitation";
    std::string mcp_server_name;
    std::string message;
    std::optional<std::string> mode;  // "form" | "url"
    std::optional<std::string> url;
    std::optional<std::string> elicitation_id;
    std::optional<std::unordered_map<std::string, std::string>> requested_schema;
};

/// Elicitation action type
enum class ElicitationAction : std::uint8_t {
    Accept,
    Decline,
    Cancel,
};

/// Response for an elicitation request
struct ControlElicitationResponse {
    ElicitationAction action;
    std::optional<std::unordered_map<std::string, std::string>> content;
};

// ============================================================================
// Control Request/Response Wrappers
// ============================================================================

/// Union of all control request inner types
using ControlRequestInner = std::variant<
    ControlInitializeRequest,
    ControlInterruptRequest,
    ControlPermissionRequest,
    ControlSetPermissionModeRequest,
    ControlSetModelRequest,
    ControlSetMaxThinkingTokensRequest,
    ControlMcpStatusRequest,
    ControlGetContextUsageRequest,
    ControlRewindFilesRequest,
    ControlCancelAsyncMessageRequest,
    ControlSeedReadStateRequest,
    HookCallbackRequest,
    ControlMcpMessageRequest,
    ControlMcpSetServersRequest,
    ControlReloadPluginsRequest,
    ControlMcpReconnectRequest,
    ControlMcpToggleRequest,
    ControlStopTaskRequest,
    ControlApplyFlagSettingsRequest,
    ControlGetSettingsRequest,
    ControlElicitationRequest
>;

/// SDK Control Request wrapper
struct ControlRequest {
    static constexpr auto type = "control_request";
    std::string request_id;
    ControlRequestInner request;
};

/// Successful control response
struct ControlSuccessResponse {
    static constexpr auto subtype = "success";
    std::string request_id;
    std::optional<std::unordered_map<std::string, std::string>> response;
};

/// Error control response
struct ControlErrorResponse {
    static constexpr auto subtype = "error";
    std::string request_id;
    std::string error;
    std::optional<std::vector<ControlRequest>> pending_permission_requests;
};

/// SDK Control Response wrapper
struct ControlResponse {
    static constexpr auto type = "control_response";
    std::variant<ControlSuccessResponse, ControlErrorResponse> response;
};

/// Cancels a currently open control request
struct ControlCancelRequest {
    static constexpr auto type = "control_cancel_request";
    std::string request_id;
};

/// Keep-alive message to maintain WebSocket connection
struct KeepAliveMessage {
    static constexpr auto type = "keep_alive";
};

/// Updates environment variables at runtime
struct UpdateEnvironmentVariablesMessage {
    static constexpr auto type = "update_environment_variables";
    std::unordered_map<std::string, std::string> variables;
};

} // namespace cc::entrypoints::control
