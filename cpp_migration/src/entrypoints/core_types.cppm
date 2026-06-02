/// @file core_types.cppm
/// @brief SDK Core Types - common serializable types for both SDK consumers and builders.
/// Migrated from src/entrypoints/sdk/coreTypes.ts and coreTypes.generated.ts
///
/// Types are derived from Zod schemas in core_schemas.cppm.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <cstdint>
#include <array>

export module cc.entrypoints.core_types;

import cc.entrypoints.core_schemas;

export namespace cc::entrypoints::core_types {

// Re-export fundamental types from core_schemas
using ModelUsage = core_schemas::ModelUsage;
using ApiKeySource = core_schemas::ApiKeySource;
using ConfigScope = core_schemas::ConfigScope;
using ThinkingConfig = core_schemas::ThinkingConfig;
using OutputFormat = core_schemas::OutputFormat;
using McpServerConfig = core_schemas::McpServerConfig;
using McpServerConfigForProcessTransport = core_schemas::McpServerConfig;
using McpServerStatus = core_schemas::McpServerStatus;
using PermissionMode = core_schemas::PermissionMode;
using PermissionUpdate = core_schemas::PermissionUpdate;
using PermissionResult = core_schemas::PermissionResult;
using HookEvent = core_schemas::HookEvent;
using ExitReason = core_schemas::ExitReason;
using HookInput = core_schemas::BaseHookInput;
using SlashCommand = core_schemas::SlashCommand;
using AgentInfo = core_schemas::AgentInfo;
using ModelInfo = core_schemas::ModelInfo;
using AccountInfo = core_schemas::AccountInfo;
using AgentDefinition = core_schemas::AgentDefinition;
using FastModeState = core_schemas::FastModeState;
using SettingSource = core_schemas::SettingSource;
using RewindFilesResult = core_schemas::RewindFilesResult;

/// Non-nullable usage (all fields guaranteed present)
using NonNullableUsage = std::unordered_map<std::string, int>;

// ============================================================================
// SDK Message Types
// ============================================================================

/// SDK user message
struct SDKUserMessage {
    static constexpr auto type = "user";
    std::unordered_map<std::string, std::string> message;  // API user message
    std::optional<std::string> parent_tool_use_id;  // nullable
    std::optional<bool> is_synthetic;
    std::optional<std::string> priority;  // "now" | "next" | "later"
    std::optional<std::string> timestamp;  // ISO timestamp
    std::optional<std::string> uuid;
    std::optional<std::string> session_id;
};

/// SDK user message replay
struct SDKUserMessageReplay {
    static constexpr auto type = "user";
    std::unordered_map<std::string, std::string> message;
    std::optional<std::string> parent_tool_use_id;
    std::optional<bool> is_synthetic;
    std::string uuid;
    std::string session_id;
    static constexpr bool is_replay = true;
};

/// SDK assistant message
struct SDKAssistantMessage {
    static constexpr auto type = "assistant";
    std::unordered_map<std::string, std::string> message;  // API assistant message
    std::optional<std::string> parent_tool_use_id;  // nullable
    std::optional<core_schemas::SDKAssistantMessageError> error;
    std::string uuid;
    std::string session_id;
};

/// SDK permission denial
struct SDKPermissionDenial {
    std::string tool_name;
    std::string tool_use_id;
    std::unordered_map<std::string, std::string> tool_input;
};

/// SDK result success
struct SDKResultSuccess {
    static constexpr auto type = "result";
    static constexpr auto subtype = "success";
    double duration_ms = 0;
    double duration_api_ms = 0;
    bool is_error = false;
    int num_turns = 0;
    std::string result;
    std::optional<std::string> stop_reason;  // nullable
    double total_cost_usd = 0.0;
    NonNullableUsage usage;
    std::unordered_map<std::string, ModelUsage> model_usage;
    std::vector<SDKPermissionDenial> permission_denials;
    std::optional<FastModeState> fast_mode_state;
    std::string uuid;
    std::string session_id;
};

/// SDK result error subtypes
enum class ResultErrorSubtype : std::uint8_t {
    ErrorDuringExecution,
    ErrorMaxTurns,
    ErrorMaxBudgetUsd,
    ErrorMaxStructuredOutputRetries,
};

/// SDK result error
struct SDKResultError {
    static constexpr auto type = "result";
    ResultErrorSubtype subtype;
    double duration_ms = 0;
    double duration_api_ms = 0;
    bool is_error = false;
    int num_turns = 0;
    std::optional<std::string> stop_reason;  // nullable
    double total_cost_usd = 0.0;
    NonNullableUsage usage;
    std::unordered_map<std::string, ModelUsage> model_usage;
    std::vector<SDKPermissionDenial> permission_denials;
    std::vector<std::string> errors;
    std::optional<FastModeState> fast_mode_state;
    std::string uuid;
    std::string session_id;
};

/// SDK result message (success or error)
using SDKResultMessage = std::variant<SDKResultSuccess, SDKResultError>;

/// SDK system message (init and other subtypes)
struct SDKSystemMessage {
    static constexpr auto type = "system";
    std::string subtype;
    std::string uuid;
    std::string session_id;
    // Additional fields depend on subtype - stored generically
    std::optional<std::string> model;
    std::optional<PermissionMode> permission_mode;
    std::optional<std::vector<std::string>> tools;
    std::optional<std::vector<std::string>> agents;
    std::optional<std::string> cwd;
    std::optional<std::string> output_style;
};

/// SDK partial assistant message (streaming)
struct SDKPartialAssistantMessage {
    static constexpr auto type = "stream_event";
    std::string event_json;  // Raw stream event as JSON string
    std::optional<std::string> parent_tool_use_id;  // nullable
    std::string uuid;
    std::string session_id;
};

/// SDK compact boundary message
struct SDKCompactBoundaryMessage {
    static constexpr auto type = "system";
    static constexpr auto subtype = "compact_boundary";
    std::string trigger;  // "manual" | "auto"
    int pre_tokens = 0;
    // Preserved segment for message relinking
    struct PreservedSegment {
        std::string head_uuid;
        std::string anchor_uuid;
        std::string tail_uuid;
    };
    std::optional<PreservedSegment> preserved_segment;
    std::string uuid;
    std::string session_id;
};

/// SDK status message
struct SDKStatusMessage {
    static constexpr auto type = "system";
    static constexpr auto subtype = "status";
    std::optional<std::string> status;  // "compacting" or null
    std::optional<PermissionMode> permission_mode;
    std::string uuid;
    std::string session_id;
};

/// SDK tool progress message
struct SDKToolProgressMessage {
    static constexpr auto type = "tool_progress";
    std::string tool_use_id;
    std::string tool_name;
    std::optional<std::string> parent_tool_use_id;  // nullable
    double elapsed_time_seconds = 0;
    std::optional<std::string> task_id;
    std::string uuid;
    std::string session_id;
};

/// SDK post-turn summary message
struct SDKPostTurnSummaryMessage {
    static constexpr auto type = "system";
    static constexpr auto subtype = "post_turn_summary";
    std::string summarizes_uuid;
    std::string status_category;  // "blocked" | "waiting" | "completed" | "review_ready" | "failed"
    std::string status_detail;
    bool is_noteworthy = false;
    std::string title;
    std::string description;
    std::string recent_action;
    std::string needs_action;
    std::vector<std::string> artifact_urls;
    std::string uuid;
    std::string session_id;
};

/// SDK streamlined text message
struct SDKStreamlinedTextMessage {
    static constexpr auto type = "streamlined_text";
    std::string text;
    std::string session_id;
    std::string uuid;
};

/// SDK streamlined tool use summary message
struct SDKStreamlinedToolUseSummaryMessage {
    static constexpr auto type = "streamlined_tool_use_summary";
    std::string tool_summary;
    std::string session_id;
    std::string uuid;
};

/// Union of all SDK message types
using SDKMessage = std::variant<
    SDKAssistantMessage,
    SDKUserMessage,
    SDKUserMessageReplay,
    SDKResultSuccess,
    SDKResultError,
    SDKSystemMessage,
    SDKPartialAssistantMessage,
    SDKCompactBoundaryMessage,
    SDKStatusMessage,
    SDKToolProgressMessage
>;

// ============================================================================
// Session Types
// ============================================================================

/// Session info metadata
struct SDKSessionInfo {
    std::string session_id;
    std::string summary;
    double last_modified = 0;  // milliseconds since epoch
    std::optional<int> file_size;
    std::optional<std::string> custom_title;
    std::optional<std::string> first_prompt;
    std::optional<std::string> git_branch;
    std::optional<std::string> cwd;
    std::optional<std::string> tag;
    std::optional<double> created_at;
};

/// Hook event names as compile-time constant array
inline constexpr std::array HOOK_EVENTS = {
    "PreToolUse", "PostToolUse", "PostToolUseFailure",
    "Notification", "UserPromptSubmit", "SessionStart", "SessionEnd",
    "Stop", "StopFailure", "SubagentStart", "SubagentStop",
    "PreCompact", "PostCompact", "PermissionRequest", "PermissionDenied",
    "Setup", "TeammateIdle", "TaskCreated", "TaskCompleted",
    "Elicitation", "ElicitationResult", "ConfigChange",
    "WorktreeCreate", "WorktreeRemove", "InstructionsLoaded",
    "CwdChanged", "FileChanged",
};

/// Exit reasons as compile-time constant array
inline constexpr std::array EXIT_REASONS = {
    "clear", "resume", "logout", "prompt_input_exit",
    "other", "bypass_permissions_disabled",
};

} // namespace cc::entrypoints::core_types
