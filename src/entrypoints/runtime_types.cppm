/// @file runtime_types.cppm
/// @brief SDK Runtime Types - non-serializable types with callbacks and interfaces.
/// Migrated from src/entrypoints/sdk/runtimeTypes.ts
///
/// These types are non-serializable (callbacks, interfaces with methods)
/// and cannot be generated from Zod schemas.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstdint>

export module cc.entrypoints.runtime_types;

import cc.entrypoints.core_types;
import cc.entrypoints.core_schemas;

export namespace cc::entrypoints::runtime {

// ============================================================================
// Effort Level
// ============================================================================

/// Named effort levels for model reasoning
enum class EffortLevel : std::uint8_t {
    Low,
    Medium,
    High,
    Max,
};

// ============================================================================
// Query Options
// ============================================================================

/// Options for a single query/conversation turn
struct Options {
    std::optional<std::string> model;
    std::optional<int> max_turns;
    std::optional<double> max_budget_usd;
    std::optional<std::string> system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::vector<std::string>> allowed_tools;
    std::optional<std::vector<std::string>> disallowed_tools;
    std::optional<std::unordered_map<std::string, core_schemas::McpServerConfig>> mcp_servers;
    std::optional<std::string> cwd;
    std::optional<core_schemas::PermissionMode> permission_mode;
    std::optional<bool> continue_conversation;
    std::optional<bool> resume_conversation;
    std::optional<std::string> output_format;  // "text" | "json" | "stream-json"
    std::optional<core_schemas::ThinkingConfig> thinking_config;
    std::optional<core_schemas::OutputFormat> json_output_schema;
    std::optional<bool> debug;
    std::optional<bool> verbose;
    std::optional<EffortLevel> effort;
    std::optional<bool> enable_remote_control;
};

/// Internal options extending Options with internal-only fields
struct InternalOptions : Options {
    std::optional<std::string> query_source;
    std::optional<std::string> parent_session_id;
    std::optional<std::string> parent_tool_use_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_type;
};

// ============================================================================
// Query Result Types
// ============================================================================

/// Result of an SDK query (wraps the async iteration result)
using SDKResultMessage = core_types::SDKResultMessage;
using SDKMessage = core_types::SDKMessage;

/// Callback type for receiving stream messages
using MessageCallback = std::function<void(const SDKMessage&)>;

/// Query handle providing abort and result access
struct QueryHandle {
    /// Abort the running query
    std::function<void()> abort;
    /// Get the final result (blocks until complete)
    std::function<SDKResultMessage()> get_result;
};

// ============================================================================
// Session Types
// ============================================================================

/// Options for creating an SDK session
struct SDKSessionOptions {
    std::optional<std::string> model;
    std::optional<int> max_turns;
    std::optional<double> max_budget_usd;
    std::optional<std::string> system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::vector<std::string>> allowed_tools;
    std::optional<std::vector<std::string>> disallowed_tools;
    std::optional<std::unordered_map<std::string, core_schemas::McpServerConfig>> mcp_servers;
    std::optional<std::string> cwd;
    std::optional<core_schemas::PermissionMode> permission_mode;
    std::optional<core_schemas::ThinkingConfig> thinking_config;
    std::optional<EffortLevel> effort;
};

/// SDK session interface for multi-turn conversations
struct SDKSession {
    std::string session_id;

    /// Send a message and get an async stream of responses
    std::function<QueryHandle(const std::string& message)> send_message;

    /// Abort the current turn
    std::function<void()> abort;
};

// ============================================================================
// Session Management Options
// ============================================================================

/// Options for listing sessions
struct ListSessionsOptions {
    std::optional<std::string> dir;
    std::optional<int> limit;
    std::optional<int> offset;
};

/// Options for getting session info
struct GetSessionInfoOptions {
    std::optional<std::string> dir;
};

/// Options for getting session messages
struct GetSessionMessagesOptions {
    std::optional<std::string> dir;
    std::optional<int> limit;
    std::optional<int> offset;
    std::optional<bool> include_system_messages;
};

/// Options for session mutations (rename, delete, etc.)
struct SessionMutationOptions {
    std::optional<std::string> dir;
};

/// Options for forking a session
struct ForkSessionOptions {
    std::optional<std::string> dir;
    std::optional<std::string> up_to_message_id;
    std::optional<std::string> title;
};

/// Result of forking a session
struct ForkSessionResult {
    std::string session_id;
};

// ============================================================================
// MCP SDK Types
// ============================================================================

/// Definition for an MCP tool exposed via the SDK transport
struct SdkMcpToolDefinition {
    std::string name;
    std::string description;
    // Input schema represented as JSON schema map
    std::unordered_map<std::string, std::string> input_schema;
    // Handler function: takes JSON args, returns JSON result
    std::function<std::string(const std::string& args_json)> handler;
    std::optional<std::unordered_map<std::string, std::string>> annotations;
    std::optional<std::string> search_hint;
    std::optional<bool> always_load;
};

/// MCP SDK server config with instance reference
struct McpSdkServerConfigWithInstance {
    static constexpr auto type = "sdk";
    std::string name;
    // Opaque server instance pointer (type-erased)
    std::shared_ptr<void> server;
};

} // namespace cc::entrypoints::runtime
