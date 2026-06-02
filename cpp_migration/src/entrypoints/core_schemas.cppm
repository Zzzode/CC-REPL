/// @file core_schemas.cppm
/// @brief SDK Core Schemas - struct definitions for serializable SDK data types.
/// Migrated from src/entrypoints/sdk/coreSchemas.ts
///
/// These schemas are the single source of truth for SDK data types.
/// Provides struct definitions equivalent to the Zod schemas.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <cstdint>
#include <array>

export module cc.entrypoints.core_schemas;

export namespace cc::entrypoints::core_schemas {

// ============================================================================
// Usage & Model Types
// ============================================================================

/// Model usage statistics
struct ModelUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_input_tokens = 0;
    int cache_creation_input_tokens = 0;
    int web_search_requests = 0;
    double cost_usd = 0.0;
    int context_window = 0;
    int max_output_tokens = 0;
};

// ============================================================================
// Config Types
// ============================================================================

/// API key source
enum class ApiKeySource : std::uint8_t {
    User,
    Project,
    Org,
    Temporary,
    OAuth,
};

/// Config scope for settings
enum class ConfigScope : std::uint8_t {
    Local,
    User,
    Project,
};

/// SDK beta version
inline constexpr auto SDK_BETA = "context-1m-2025-08-07";

// ============================================================================
// Thinking Config Types
// ============================================================================

/// Adaptive thinking - Claude decides when and how much to think
struct ThinkingAdaptive {
    static constexpr auto type = "adaptive";
};

/// Fixed thinking token budget (older models)
struct ThinkingEnabled {
    static constexpr auto type = "enabled";
    std::optional<int> budget_tokens;
};

/// No extended thinking
struct ThinkingDisabled {
    static constexpr auto type = "disabled";
};

/// Controls Claude's thinking/reasoning behavior
using ThinkingConfig = std::variant<ThinkingAdaptive, ThinkingEnabled, ThinkingDisabled>;

// ============================================================================
// Output Format Types
// ============================================================================

/// JSON schema output format
struct JsonSchemaOutputFormat {
    static constexpr auto type = "json_schema";
    std::unordered_map<std::string, std::string> schema;
};

using OutputFormat = JsonSchemaOutputFormat;

// ============================================================================
// MCP Server Config Types
// ============================================================================

/// Stdio transport MCP server config
struct McpStdioServerConfig {
    static constexpr auto type = "stdio";
    std::string command;
    std::optional<std::vector<std::string>> args;
    std::optional<std::unordered_map<std::string, std::string>> env;
};

/// SSE transport MCP server config
struct McpSSEServerConfig {
    static constexpr auto type = "sse";
    std::string url;
    std::optional<std::unordered_map<std::string, std::string>> headers;
};

/// HTTP transport MCP server config
struct McpHttpServerConfig {
    static constexpr auto type = "http";
    std::string url;
    std::optional<std::unordered_map<std::string, std::string>> headers;
};

/// SDK transport MCP server config
struct McpSdkServerConfig {
    static constexpr auto type = "sdk";
    std::string name;
};

/// Union of process-transport MCP server configurations
using McpServerConfig = std::variant<
    McpStdioServerConfig,
    McpSSEServerConfig,
    McpHttpServerConfig,
    McpSdkServerConfig
>;

/// Claude AI proxy server config (output-only)
struct McpClaudeAIProxyServerConfig {
    static constexpr auto type = "claudeai-proxy";
    std::string url;
    std::string id;
};

/// Tool annotation in MCP server status
struct McpToolAnnotation {
    std::optional<bool> read_only;
    std::optional<bool> destructive;
    std::optional<bool> open_world;
};

/// Tool provided by an MCP server
struct McpServerTool {
    std::string name;
    std::optional<std::string> description;
    std::optional<McpToolAnnotation> annotations;
};

/// Server info returned when connected
struct McpServerInfo {
    std::string name;
    std::string version;
};

/// Server capabilities
struct McpServerCapabilities {
    std::optional<std::unordered_map<std::string, std::string>> experimental;
};

/// Status of an MCP server connection
enum class McpConnectionStatus : std::uint8_t {
    Connected,
    Failed,
    NeedsAuth,
    Pending,
    Disabled,
};

/// Full MCP server status
struct McpServerStatus {
    std::string name;
    McpConnectionStatus status;
    std::optional<McpServerInfo> server_info;
    std::optional<std::string> error;
    std::optional<McpServerConfig> config;
    std::optional<std::string> scope;
    std::optional<std::vector<McpServerTool>> tools;
    std::optional<McpServerCapabilities> capabilities;
};

// ============================================================================
// Permission Types
// ============================================================================

/// Destination for permission updates
enum class PermissionUpdateDestination : std::uint8_t {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    Session,
    CliArg,
};

/// Permission behavior
enum class PermissionBehavior : std::uint8_t {
    Allow,
    Deny,
    Ask,
};

/// Permission rule value
struct PermissionRuleValue {
    std::string tool_name;
    std::optional<std::string> rule_content;
};

/// Permission mode for controlling tool execution
enum class PermissionMode : std::uint8_t {
    Default,
    AcceptEdits,
    BypassPermissions,
    Plan,
    DontAsk,
};

/// Add rules permission update
struct PermissionAddRules {
    std::vector<PermissionRuleValue> rules;
    PermissionBehavior behavior;
    PermissionUpdateDestination destination;
};

/// Replace rules permission update
struct PermissionReplaceRules {
    std::vector<PermissionRuleValue> rules;
    PermissionBehavior behavior;
    PermissionUpdateDestination destination;
};

/// Remove rules permission update
struct PermissionRemoveRules {
    std::vector<PermissionRuleValue> rules;
    PermissionBehavior behavior;
    PermissionUpdateDestination destination;
};

/// Set mode permission update
struct PermissionSetMode {
    PermissionMode mode;
    PermissionUpdateDestination destination;
};

/// Add directories permission update
struct PermissionAddDirectories {
    std::vector<std::string> directories;
    PermissionUpdateDestination destination;
};

/// Remove directories permission update
struct PermissionRemoveDirectories {
    std::vector<std::string> directories;
    PermissionUpdateDestination destination;
};

/// Discriminated union of permission updates
using PermissionUpdate = std::variant<
    PermissionAddRules,
    PermissionReplaceRules,
    PermissionRemoveRules,
    PermissionSetMode,
    PermissionAddDirectories,
    PermissionRemoveDirectories
>;

/// Classification of permission decision for telemetry
enum class PermissionDecisionClassification : std::uint8_t {
    UserTemporary,
    UserPermanent,
    UserReject,
};

/// Permission allow result
struct PermissionAllowResult {
    static constexpr auto behavior = "allow";
    std::optional<std::unordered_map<std::string, std::string>> updated_input;
    std::optional<std::vector<PermissionUpdate>> updated_permissions;
    std::optional<std::string> tool_use_id;
    std::optional<PermissionDecisionClassification> decision_classification;
};

/// Permission deny result
struct PermissionDenyResult {
    static constexpr auto behavior = "deny";
    std::string message;
    std::optional<bool> interrupt;
    std::optional<std::string> tool_use_id;
    std::optional<PermissionDecisionClassification> decision_classification;
};

/// Permission result (allow or deny)
using PermissionResult = std::variant<PermissionAllowResult, PermissionDenyResult>;

// ============================================================================
// Hook Types
// ============================================================================

/// Hook event types
enum class HookEvent : std::uint8_t {
    PreToolUse,
    PostToolUse,
    PostToolUseFailure,
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
    PermissionDenied,
    Setup,
    TeammateIdle,
    TaskCreated,
    TaskCompleted,
    Elicitation,
    ElicitationResult,
    ConfigChange,
    WorktreeCreate,
    WorktreeRemove,
    InstructionsLoaded,
    CwdChanged,
    FileChanged,
};

/// Exit reasons for session end
enum class ExitReason : std::uint8_t {
    Clear,
    Resume,
    Logout,
    PromptInputExit,
    Other,
    BypassPermissionsDisabled,
};

/// Base hook input fields (shared across all hook events)
struct BaseHookInput {
    std::string session_id;
    std::string transcript_path;
    std::string cwd;
    std::optional<std::string> permission_mode;
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_type;
};

// ============================================================================
// Skill/Command Types
// ============================================================================

/// Slash command (skill) info
struct SlashCommand {
    std::string name;
    std::string description;
    std::string argument_hint;
};

/// Agent info
struct AgentInfo {
    std::string name;
    std::string description;
    std::optional<std::string> model;
};

/// Model info
struct ModelInfo {
    std::string value;
    std::string display_name;
    std::string description;
    std::optional<bool> supports_effort;
    std::optional<std::vector<std::string>> supported_effort_levels;
    std::optional<bool> supports_adaptive_thinking;
    std::optional<bool> supports_fast_mode;
    std::optional<bool> supports_auto_mode;
};

/// Account info
struct AccountInfo {
    std::optional<std::string> email;
    std::optional<std::string> organization;
    std::optional<std::string> subscription_type;
    std::optional<std::string> token_source;
    std::optional<std::string> api_key_source;
    std::optional<std::string> api_provider;  // "firstParty" | "bedrock" | "vertex" | "foundry"
};

/// Agent MCP server spec (can be a name string or inline config map)
using AgentMcpServerSpec = std::variant<
    std::string,
    std::unordered_map<std::string, McpServerConfig>
>;

/// Memory scope for agent definition
enum class AgentMemoryScope : std::uint8_t {
    User,
    Project,
    Local,
};

/// Agent definition for custom subagents
struct AgentDefinition {
    std::string description;
    std::optional<std::vector<std::string>> tools;
    std::optional<std::vector<std::string>> disallowed_tools;
    std::string prompt;
    std::optional<std::string> model;
    std::optional<std::vector<AgentMcpServerSpec>> mcp_servers;
    std::optional<std::string> critical_system_reminder_experimental;
    std::optional<std::vector<std::string>> skills;
    std::optional<std::string> initial_prompt;
    std::optional<int> max_turns;
    std::optional<bool> background;
    std::optional<AgentMemoryScope> memory;
    std::optional<std::string> effort;  // named level or integer as string
    std::optional<PermissionMode> permission_mode;
};

/// Fast mode state
enum class FastModeState : std::uint8_t {
    Off,
    Cooldown,
    On,
};

// ============================================================================
// SDK Message Error Types
// ============================================================================

/// Assistant message error types
enum class SDKAssistantMessageError : std::uint8_t {
    AuthenticationFailed,
    BillingError,
    RateLimit,
    InvalidRequest,
    ServerError,
    Unknown,
    MaxOutputTokens,
};

// ============================================================================
// Settings Types
// ============================================================================

/// Setting source (file-based settings location)
enum class SettingSource : std::uint8_t {
    User,
    Project,
    Local,
};

/// SDK Plugin configuration
struct SdkPluginConfig {
    static constexpr auto type = "local";
    std::string path;
};

/// Rewind files result
struct RewindFilesResult {
    bool can_rewind = false;
    std::optional<std::string> error;
    std::optional<std::vector<std::string>> files_changed;
    std::optional<int> insertions;
    std::optional<int> deletions;
};

} // namespace cc::entrypoints::core_schemas
