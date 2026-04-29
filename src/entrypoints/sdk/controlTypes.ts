// Stubbed control protocol types for leaked-source compatibility.
//
// These types are hand-derived from the Zod schemas in controlSchemas.ts.

import type {
  AccountInfo,
  AgentDefinition,
  AgentInfo,
  FastModeState,
  HookEvent,
  HookInput,
  McpServerConfigForProcessTransport,
  McpServerStatus,
  ModelInfo,
  PermissionMode,
  PermissionUpdate,
  SDKMessage,
  SDKPartialAssistantMessage,
  SDKPostTurnSummaryMessage,
  SDKStreamlinedTextMessage,
  SDKStreamlinedToolUseSummaryMessage,
  SDKUserMessage,
  SlashCommand,
} from './coreTypes.js'

// ============================================================================
// Control Request Inner Types
// ============================================================================

export type SDKControlInitializeRequest = {
  subtype: 'initialize'
  hooks?: Record<HookEvent, Array<{ matcher?: string; hookCallbackIds: string[]; timeout?: number }>>
  sdkMcpServers?: string[]
  jsonSchema?: Record<string, unknown>
  systemPrompt?: string
  appendSystemPrompt?: string
  agents?: Record<string, AgentDefinition>
  promptSuggestions?: boolean
  agentProgressSummaries?: boolean
}

export type SDKControlInterruptRequest = {
  subtype: 'interrupt'
}

export type SDKControlPermissionRequest = {
  subtype: 'can_use_tool'
  tool_name: string
  input: Record<string, unknown>
  permission_suggestions?: PermissionUpdate[]
  blocked_path?: string
  decision_reason?: string
  title?: string
  display_name?: string
  tool_use_id: string
  agent_id?: string
  description?: string
}

export type SDKControlSetPermissionModeRequest = {
  subtype: 'set_permission_mode'
  mode: PermissionMode
  ultraplan?: boolean
}

export type SDKControlSetModelRequest = {
  subtype: 'set_model'
  model?: string
}

export type SDKControlSetMaxThinkingTokensRequest = {
  subtype: 'set_max_thinking_tokens'
  max_thinking_tokens: number | null
}

export type SDKControlMcpStatusRequest = {
  subtype: 'mcp_status'
}

export type SDKControlGetContextUsageRequest = {
  subtype: 'get_context_usage'
}

export type SDKControlRewindFilesRequest = {
  subtype: 'rewind_files'
  user_message_id: string
  dry_run?: boolean
}

export type SDKControlCancelAsyncMessageRequest = {
  subtype: 'cancel_async_message'
  message_uuid: string
}

export type SDKControlSeedReadStateRequest = {
  subtype: 'seed_read_state'
  path: string
  mtime: number
}

export type SDKHookCallbackRequest = {
  subtype: 'hook_callback'
  callback_id: string
  input: HookInput
  tool_use_id?: string
}

export type SDKControlMcpMessageRequest = {
  subtype: 'mcp_message'
  server_name: string
  message: unknown
}

export type SDKControlMcpSetServersRequest = {
  subtype: 'mcp_set_servers'
  servers: Record<string, McpServerConfigForProcessTransport>
}

export type SDKControlReloadPluginsRequest = {
  subtype: 'reload_plugins'
}

export type SDKControlMcpReconnectRequest = {
  subtype: 'mcp_reconnect'
  serverName: string
}

export type SDKControlMcpToggleRequest = {
  subtype: 'mcp_toggle'
  serverName: string
  enabled: boolean
}

export type SDKControlStopTaskRequest = {
  subtype: 'stop_task'
  task_id: string
}

export type SDKControlApplyFlagSettingsRequest = {
  subtype: 'apply_flag_settings'
  settings: Record<string, unknown>
}

export type SDKControlGetSettingsRequest = {
  subtype: 'get_settings'
}

export type SDKControlElicitationRequest = {
  subtype: 'elicitation'
  mcp_server_name: string
  message: string
  mode?: 'form' | 'url'
  url?: string
  elicitation_id?: string
  requested_schema?: Record<string, unknown>
}

export type SDKControlEndSessionRequest = {
  subtype: 'end_session'
  reason?: string
}

export type SDKControlChannelEnableRequest = {
  subtype: 'channel_enable'
  serverName: string
}

export type SDKControlMcpAuthenticateRequest = {
  subtype: 'mcp_authenticate'
  serverName: string
  authorizationUrl: string
  oauth?: Record<string, unknown>
}

export type SDKControlMcpOAuthCallbackUrlRequest = {
  subtype: 'mcp_oauth_callback_url'
  serverName: string
  callbackUrl: string
}

export type SDKControlRequestInner =
  | SDKControlInitializeRequest
  | SDKControlInterruptRequest
  | SDKControlPermissionRequest
  | SDKControlSetPermissionModeRequest
  | SDKControlSetModelRequest
  | SDKControlSetMaxThinkingTokensRequest
  | SDKControlMcpStatusRequest
  | SDKControlGetContextUsageRequest
  | SDKControlRewindFilesRequest
  | SDKControlCancelAsyncMessageRequest
  | SDKControlSeedReadStateRequest
  | SDKHookCallbackRequest
  | SDKControlMcpMessageRequest
  | SDKControlMcpSetServersRequest
  | SDKControlReloadPluginsRequest
  | SDKControlMcpReconnectRequest
  | SDKControlMcpToggleRequest
  | SDKControlStopTaskRequest
  | SDKControlApplyFlagSettingsRequest
  | SDKControlGetSettingsRequest
  | SDKControlElicitationRequest
  | SDKControlEndSessionRequest
  | SDKControlChannelEnableRequest
  | SDKControlMcpAuthenticateRequest
  | SDKControlMcpOAuthCallbackUrlRequest

// ============================================================================
// Control Request/Response Wrappers
// ============================================================================

export type SDKControlRequest = {
  type: 'control_request'
  request_id: string
  request: SDKControlRequestInner
}

type ControlSuccessResponse = {
  subtype: 'success'
  request_id: string
  response?: Record<string, unknown>
}

type ControlErrorResponse = {
  subtype: 'error'
  request_id: string
  error: string
  pending_permission_requests?: SDKControlRequest[]
}

export type SDKControlResponse = {
  type: 'control_response'
  response: ControlSuccessResponse | ControlErrorResponse
}

export type SDKControlCancelRequest = {
  type: 'control_cancel_request'
  request_id: string
}

// ============================================================================
// Control Response Inner Types
// ============================================================================

export type SDKControlInitializeResponse = {
  commands: SlashCommand[]
  agents: AgentInfo[]
  output_style: string
  available_output_styles: string[]
  models: ModelInfo[]
  account: AccountInfo
  pid?: number
  fast_mode_state?: FastModeState
}

export type SDKControlMcpStatusResponse = {
  mcpServers: McpServerStatus[]
}

export type SDKControlRewindFilesResponse = {
  canRewind: boolean
  error?: string
  filesChanged?: string[]
  insertions?: number
  deletions?: number
}

export type SDKControlCancelAsyncMessageResponse = {
  cancelled: boolean
}

export type SDKControlMcpSetServersResponse = {
  added: string[]
  removed: string[]
  errors: Record<string, string>
}

export type SDKControlReloadPluginsResponse = {
  commands: SlashCommand[]
  agents: AgentInfo[]
  plugins: Array<{ name: string; path: string; source?: string }>
  mcpServers: McpServerStatus[]
  error_count: number
}

export type SDKControlGetSettingsResponse = {
  effective: Record<string, unknown>
  sources: Array<{
    source: 'userSettings' | 'projectSettings' | 'localSettings' | 'flagSettings' | 'policySettings'
    settings: Record<string, unknown>
  }>
  applied?: {
    model: string
    effort: 'low' | 'medium' | 'high' | 'max' | null
  }
}

export type SDKControlElicitationResponse = {
  action: 'accept' | 'decline' | 'cancel'
  content?: Record<string, unknown>
}

// ============================================================================
// Aggregate Message Types
// ============================================================================

export type StdoutMessage =
  | SDKMessage
  | SDKStreamlinedTextMessage
  | SDKStreamlinedToolUseSummaryMessage
  | SDKPostTurnSummaryMessage
  | SDKControlResponse
  | SDKControlRequest
  | SDKControlCancelRequest
  | { type: 'keep_alive' }

export type StdinMessage =
  | SDKUserMessage
  | SDKControlRequest
  | SDKControlResponse
  | { type: 'keep_alive' }
  | { type: 'update_environment_variables'; variables: Record<string, string> }

export type { SDKPartialAssistantMessage }
