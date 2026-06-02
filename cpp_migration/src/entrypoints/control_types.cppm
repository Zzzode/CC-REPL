/// @file control_types.cppm
/// @brief Control protocol types derived from Zod schemas in controlSchemas.
/// Migrated from src/entrypoints/sdk/controlTypes.ts
///
/// These types are the TypeScript-equivalent type aliases for the control protocol.
/// In C++ they largely mirror control_schemas but provide convenient type aliases.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <cstdint>

export module cc.entrypoints.control_types;

import cc.entrypoints.core_types;
import cc.entrypoints.control_schemas;

export namespace cc::entrypoints::control_types {

// Re-export control schemas types as canonical control types
using SDKControlInitializeRequest = control::ControlInitializeRequest;
using SDKControlInterruptRequest = control::ControlInterruptRequest;
using SDKControlPermissionRequest = control::ControlPermissionRequest;
using SDKControlSetPermissionModeRequest = control::ControlSetPermissionModeRequest;
using SDKControlSetModelRequest = control::ControlSetModelRequest;
using SDKControlSetMaxThinkingTokensRequest = control::ControlSetMaxThinkingTokensRequest;
using SDKControlMcpStatusRequest = control::ControlMcpStatusRequest;
using SDKControlGetContextUsageRequest = control::ControlGetContextUsageRequest;
using SDKControlRewindFilesRequest = control::ControlRewindFilesRequest;
using SDKControlCancelAsyncMessageRequest = control::ControlCancelAsyncMessageRequest;
using SDKControlSeedReadStateRequest = control::ControlSeedReadStateRequest;
using SDKHookCallbackRequest = control::HookCallbackRequest;
using SDKControlMcpMessageRequest = control::ControlMcpMessageRequest;
using SDKControlMcpSetServersRequest = control::ControlMcpSetServersRequest;
using SDKControlReloadPluginsRequest = control::ControlReloadPluginsRequest;
using SDKControlMcpReconnectRequest = control::ControlMcpReconnectRequest;
using SDKControlMcpToggleRequest = control::ControlMcpToggleRequest;
using SDKControlStopTaskRequest = control::ControlStopTaskRequest;
using SDKControlApplyFlagSettingsRequest = control::ControlApplyFlagSettingsRequest;
using SDKControlGetSettingsRequest = control::ControlGetSettingsRequest;
using SDKControlElicitationRequest = control::ControlElicitationRequest;

// Additional request types not in schemas (runtime-only)

/// End session request
struct SDKControlEndSessionRequest {
    static constexpr auto subtype = "end_session";
    std::optional<std::string> reason;
};

/// Channel enable request
struct SDKControlChannelEnableRequest {
    static constexpr auto subtype = "channel_enable";
    std::string server_name;
};

/// MCP authenticate request
struct SDKControlMcpAuthenticateRequest {
    static constexpr auto subtype = "mcp_authenticate";
    std::string server_name;
    std::string authorization_url;
    std::optional<std::unordered_map<std::string, std::string>> oauth;
};

/// MCP OAuth callback URL request
struct SDKControlMcpOAuthCallbackUrlRequest {
    static constexpr auto subtype = "mcp_oauth_callback_url";
    std::string server_name;
    std::string callback_url;
};

/// Union of all control request inner types (extended from schemas)
using SDKControlRequestInner = std::variant<
    SDKControlInitializeRequest,
    SDKControlInterruptRequest,
    SDKControlPermissionRequest,
    SDKControlSetPermissionModeRequest,
    SDKControlSetModelRequest,
    SDKControlSetMaxThinkingTokensRequest,
    SDKControlMcpStatusRequest,
    SDKControlGetContextUsageRequest,
    SDKControlRewindFilesRequest,
    SDKControlCancelAsyncMessageRequest,
    SDKControlSeedReadStateRequest,
    SDKHookCallbackRequest,
    SDKControlMcpMessageRequest,
    SDKControlMcpSetServersRequest,
    SDKControlReloadPluginsRequest,
    SDKControlMcpReconnectRequest,
    SDKControlMcpToggleRequest,
    SDKControlStopTaskRequest,
    SDKControlApplyFlagSettingsRequest,
    SDKControlGetSettingsRequest,
    SDKControlElicitationRequest,
    SDKControlEndSessionRequest,
    SDKControlChannelEnableRequest,
    SDKControlMcpAuthenticateRequest,
    SDKControlMcpOAuthCallbackUrlRequest
>;

// ============================================================================
// Control Request/Response Wrappers
// ============================================================================

/// SDK Control Request (wraps inner with type and request_id)
using SDKControlRequest = control::ControlRequest;

/// SDK Control Response
using SDKControlResponse = control::ControlResponse;

/// SDK Control Cancel Request
using SDKControlCancelRequest = control::ControlCancelRequest;

// ============================================================================
// Control Response Inner Types
// ============================================================================

using SDKControlInitializeResponse = control::ControlInitializeResponse;
using SDKControlMcpStatusResponse = control::ControlMcpStatusResponse;
using SDKControlRewindFilesResponse = control::ControlRewindFilesResponse;
using SDKControlCancelAsyncMessageResponse = control::ControlCancelAsyncMessageResponse;
using SDKControlMcpSetServersResponse = control::ControlMcpSetServersResponse;
using SDKControlReloadPluginsResponse = control::ControlReloadPluginsResponse;
using SDKControlGetSettingsResponse = control::ControlGetSettingsResponse;
using SDKControlElicitationResponse = control::ControlElicitationResponse;

// ============================================================================
// Aggregate Message Types
// ============================================================================

/// Messages written to stdout by the CLI
using StdoutMessage = std::variant<
    core_types::SDKMessage,
    core_types::SDKStreamlinedTextMessage,
    core_types::SDKStreamlinedToolUseSummaryMessage,
    core_types::SDKPostTurnSummaryMessage,
    SDKControlResponse,
    SDKControlRequest,
    SDKControlCancelRequest,
    control::KeepAliveMessage
>;

/// Messages read from stdin by the CLI
using StdinMessage = std::variant<
    core_types::SDKUserMessage,
    SDKControlRequest,
    SDKControlResponse,
    control::KeepAliveMessage,
    control::UpdateEnvironmentVariablesMessage
>;

} // namespace cc::entrypoints::control_types
