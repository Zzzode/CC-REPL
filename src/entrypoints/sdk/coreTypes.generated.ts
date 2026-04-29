// Stubbed generated SDK types for leaked-source compatibility.
//
// These types are hand-derived from the Zod schemas in coreSchemas.ts.
// The real build generates them via scripts/generate-sdk-types.ts.

// ============================================================================
// Usage & Model Types
// ============================================================================

export type ModelUsage = {
  inputTokens: number
  outputTokens: number
  cacheReadInputTokens: number
  cacheCreationInputTokens: number
  webSearchRequests: number
  costUSD: number
  contextWindow: number
  maxOutputTokens: number
}

export type NonNullableUsage = Record<string, number>

// ============================================================================
// Config Types
// ============================================================================

export type ApiKeySource = 'user' | 'project' | 'org' | 'temporary' | 'oauth'

export type ConfigScope = 'local' | 'user' | 'project'

export type SdkBeta = 'context-1m-2025-08-07'

export type ThinkingConfig =
  | { type: 'adaptive' }
  | { type: 'enabled'; budgetTokens?: number }
  | { type: 'disabled' }

export type OutputFormat = {
  type: 'json_schema'
  schema: Record<string, unknown>
}

// ============================================================================
// MCP Types
// ============================================================================

export type McpStdioServerConfig = {
  type?: 'stdio'
  command: string
  args?: string[]
  env?: Record<string, string>
}

export type McpSSEServerConfig = {
  type: 'sse'
  url: string
  headers?: Record<string, string>
}

export type McpHttpServerConfig = {
  type: 'http'
  url: string
  headers?: Record<string, string>
}

export type McpSdkServerConfig = {
  type: 'sdk'
  name: string
}

export type McpServerConfigForProcessTransport =
  | McpStdioServerConfig
  | McpSSEServerConfig
  | McpHttpServerConfig
  | McpSdkServerConfig

export type McpServerStatus = {
  name: string
  status: 'connected' | 'failed' | 'needs-auth' | 'pending' | 'disabled'
  serverInfo?: { name: string; version: string }
  error?: string
  config?: McpServerConfigForProcessTransport | { type: 'claudeai-proxy'; url: string; id: string }
  scope?: string
  tools?: Array<{
    name: string
    description?: string
    annotations?: { readOnly?: boolean; destructive?: boolean; openWorld?: boolean }
  }>
  capabilities?: { experimental?: Record<string, unknown> }
}

// ============================================================================
// Permission Types
// ============================================================================

export type PermissionUpdateDestination =
  | 'userSettings'
  | 'projectSettings'
  | 'localSettings'
  | 'session'
  | 'cliArg'

export type PermissionBehavior = 'allow' | 'deny' | 'ask'

export type PermissionRuleValue = {
  toolName: string
  ruleContent?: string
}

export type PermissionUpdate =
  | { type: 'addRules'; rules: PermissionRuleValue[]; behavior: PermissionBehavior; destination: PermissionUpdateDestination }
  | { type: 'replaceRules'; rules: PermissionRuleValue[]; behavior: PermissionBehavior; destination: PermissionUpdateDestination }
  | { type: 'removeRules'; rules: PermissionRuleValue[]; behavior: PermissionBehavior; destination: PermissionUpdateDestination }
  | { type: 'setMode'; mode: PermissionMode; destination: PermissionUpdateDestination }
  | { type: 'addDirectories'; directories: string[]; destination: PermissionUpdateDestination }
  | { type: 'removeDirectories'; directories: string[]; destination: PermissionUpdateDestination }

export type PermissionDecisionClassification =
  | 'user_temporary'
  | 'user_permanent'
  | 'user_reject'

export type PermissionResult =
  | {
      behavior: 'allow'
      updatedInput?: Record<string, unknown>
      updatedPermissions?: PermissionUpdate[]
      toolUseID?: string
      decisionClassification?: PermissionDecisionClassification
    }
  | {
      behavior: 'deny'
      message: string
      interrupt?: boolean
      toolUseID?: string
      decisionClassification?: PermissionDecisionClassification
    }

export type PermissionMode =
  | 'default'
  | 'acceptEdits'
  | 'bypassPermissions'
  | 'plan'
  | 'dontAsk'

// ============================================================================
// Hook Types
// ============================================================================

export type HookEvent =
  | 'PreToolUse'
  | 'PostToolUse'
  | 'PostToolUseFailure'
  | 'Notification'
  | 'UserPromptSubmit'
  | 'SessionStart'
  | 'SessionEnd'
  | 'Stop'
  | 'StopFailure'
  | 'SubagentStart'
  | 'SubagentStop'
  | 'PreCompact'
  | 'PostCompact'
  | 'PermissionRequest'
  | 'PermissionDenied'
  | 'Setup'
  | 'TeammateIdle'
  | 'TaskCreated'
  | 'TaskCompleted'
  | 'Elicitation'
  | 'ElicitationResult'
  | 'ConfigChange'
  | 'WorktreeCreate'
  | 'WorktreeRemove'
  | 'InstructionsLoaded'
  | 'CwdChanged'
  | 'FileChanged'

export type ExitReason =
  | 'clear'
  | 'resume'
  | 'logout'
  | 'prompt_input_exit'
  | 'other'
  | 'bypass_permissions_disabled'

export type HookInput = {
  session_id: string
  transcript_path: string
  cwd: string
  permission_mode?: string
  agent_id?: string
  agent_type?: string
  hook_event_name: string
  [key: string]: unknown
}

export type HookJSONOutput =
  | { hookEventName?: string; [key: string]: unknown }
  | Record<string, unknown>

// ============================================================================
// Command/Agent/Model Info Types
// ============================================================================

export type SlashCommand = {
  name: string
  description: string
  argumentHint: string
}

export type AgentInfo = {
  name: string
  description: string
  model?: string
}

export type ModelInfo = {
  value: string
  displayName: string
  description: string
  supportsEffort?: boolean
  supportedEffortLevels?: Array<'low' | 'medium' | 'high' | 'max'>
  supportsAdaptiveThinking?: boolean
  supportsFastMode?: boolean
  supportsAutoMode?: boolean
}

export type AccountInfo = {
  email?: string
  organization?: string
  subscriptionType?: string
  tokenSource?: string
  apiKeySource?: string
  apiProvider?: 'firstParty' | 'bedrock' | 'vertex' | 'foundry'
}

export type AgentDefinition = {
  description: string
  tools?: string[]
  disallowedTools?: string[]
  prompt: string
  model?: string
  mcpServers?: Array<string | Record<string, McpServerConfigForProcessTransport>>
  criticalSystemReminder_EXPERIMENTAL?: string
  skills?: string[]
  initialPrompt?: string
  maxTurns?: number
  background?: boolean
  memory?: 'user' | 'project' | 'local'
  effort?: 'low' | 'medium' | 'high' | 'max' | number
  permissionMode?: PermissionMode
}

export type FastModeState = 'off' | 'cooldown' | 'on'

// ============================================================================
// SDK Message Types
// ============================================================================

export type SDKAssistantMessageError =
  | 'authentication_failed'
  | 'billing_error'
  | 'rate_limit'
  | 'invalid_request'
  | 'server_error'
  | 'unknown'
  | 'max_output_tokens'

export type SDKStatus = 'compacting' | null

export type SDKUserMessage = {
  type: 'user'
  message: Record<string, unknown>
  parent_tool_use_id: string | null
  isSynthetic?: boolean
  tool_use_result?: unknown
  priority?: 'now' | 'next' | 'later'
  timestamp?: string
  uuid?: string
  session_id?: string
}

export type SDKUserMessageReplay = SDKUserMessage & {
  uuid: string
  session_id: string
  isReplay: true
}

export type SDKAssistantMessage = {
  type: 'assistant'
  message: Record<string, unknown>
  parent_tool_use_id: string | null
  error?: SDKAssistantMessageError
  uuid: string
  session_id: string
}

export type SDKPermissionDenial = {
  tool_name: string
  tool_use_id: string
  tool_input: Record<string, unknown>
}

export type SDKResultSuccess = {
  type: 'result'
  subtype: 'success'
  duration_ms: number
  duration_api_ms: number
  is_error: boolean
  num_turns: number
  result: string
  stop_reason: string | null
  total_cost_usd: number
  usage: NonNullableUsage
  modelUsage: Record<string, ModelUsage>
  permission_denials: SDKPermissionDenial[]
  structured_output?: unknown
  fast_mode_state?: FastModeState
  uuid: string
  session_id: string
}

export type SDKResultError = {
  type: 'result'
  subtype:
    | 'error_during_execution'
    | 'error_max_turns'
    | 'error_max_budget_usd'
    | 'error_max_structured_output_retries'
  duration_ms: number
  duration_api_ms: number
  is_error: boolean
  num_turns: number
  stop_reason: string | null
  total_cost_usd: number
  usage: NonNullableUsage
  modelUsage: Record<string, ModelUsage>
  permission_denials: SDKPermissionDenial[]
  errors: string[]
  fast_mode_state?: FastModeState
  uuid: string
  session_id: string
}

export type SDKResultMessage = SDKResultSuccess | SDKResultError

export type SDKSystemMessage = {
  type: 'system'
  subtype: string
  uuid: string
  session_id: string
  [key: string]: unknown
}

export type SDKPartialAssistantMessage = {
  type: 'stream_event'
  event: unknown
  parent_tool_use_id: string | null
  uuid: string
  session_id: string
}

export type SDKCompactBoundaryMessage = {
  type: 'system'
  subtype: 'compact_boundary'
  compact_metadata: {
    trigger: 'manual' | 'auto'
    pre_tokens: number
    preserved_segment?: {
      head_uuid: string
      anchor_uuid: string
      tail_uuid: string
    }
  }
  uuid: string
  session_id: string
}

export type SDKStatusMessage = {
  type: 'system'
  subtype: 'status'
  status: SDKStatus
  permissionMode?: PermissionMode
  uuid: string
  session_id: string
}

export type SDKToolProgressMessage = {
  type: 'tool_progress'
  tool_use_id: string
  tool_name: string
  parent_tool_use_id: string | null
  elapsed_time_seconds: number
  task_id?: string
  uuid: string
  session_id: string
}

export type SDKPostTurnSummaryMessage = {
  type: 'system'
  subtype: 'post_turn_summary'
  summarizes_uuid: string
  status_category: 'blocked' | 'waiting' | 'completed' | 'review_ready' | 'failed'
  status_detail: string
  is_noteworthy: boolean
  title: string
  description: string
  recent_action: string
  needs_action: string
  artifact_urls: string[]
  uuid: string
  session_id: string
}

export type SDKStreamlinedTextMessage = {
  type: 'streamlined_text'
  text: string
  session_id: string
  uuid: string
}

export type SDKStreamlinedToolUseSummaryMessage = {
  type: 'streamlined_tool_use_summary'
  tool_summary: string
  session_id: string
  uuid: string
}

export type SDKMessage =
  | SDKAssistantMessage
  | SDKUserMessage
  | SDKUserMessageReplay
  | SDKResultMessage
  | SDKSystemMessage
  | SDKPartialAssistantMessage
  | SDKCompactBoundaryMessage
  | SDKStatusMessage
  | SDKToolProgressMessage

// ============================================================================
// Session Types
// ============================================================================

export type SDKSessionInfo = {
  sessionId: string
  summary: string
  lastModified: number
  fileSize?: number
  customTitle?: string
  firstPrompt?: string
  gitBranch?: string
  cwd?: string
  tag?: string
  createdAt?: number
}

export type SessionMessage = {
  type: string
  uuid?: string
  session_id?: string
  message?: Record<string, unknown>
  [key: string]: unknown
}

// ============================================================================
// Settings Types
// ============================================================================

export type SettingSource = 'user' | 'project' | 'local'

// ============================================================================
// Rewind Files
// ============================================================================

export type RewindFilesResult = {
  canRewind: boolean
  error?: string
  filesChanged?: string[]
  insertions?: number
  deletions?: number
}
