// Stubbed runtime SDK types for leaked-source compatibility.
//
// These types are non-serializable (callbacks, interfaces with methods)
// and cannot be generated from Zod schemas.

import type {
  McpServerConfigForProcessTransport,
  OutputFormat,
  PermissionMode,
  SdkBeta,
  SDKMessage,
  SDKResultMessage,
  SDKUserMessage,
  ThinkingConfig,
} from './coreTypes.js'

// ============================================================================
// Zod-related Utility Types
// ============================================================================

export type AnyZodRawShape = Record<string, { _output: unknown }>

export type InferShape<T extends AnyZodRawShape> = {
  [K in keyof T]: T[K]['_output']
}

// ============================================================================
// Effort
// ============================================================================

export type EffortLevel = 'low' | 'medium' | 'high' | 'max'

// ============================================================================
// MCP SDK Types
// ============================================================================

export type SdkMcpToolDefinition<T extends AnyZodRawShape = AnyZodRawShape> = {
  name: string
  description: string
  inputSchema: T
  handler: (args: InferShape<T>, extra: unknown) => Promise<unknown>
  annotations?: Record<string, unknown>
  searchHint?: string
  alwaysLoad?: boolean
}

export type McpSdkServerConfigWithInstance = {
  type: 'sdk'
  name: string
  server: unknown
}

// ============================================================================
// Query Options
// ============================================================================

export type Options = {
  model?: string
  maxTurns?: number
  maxBudgetUsd?: number
  systemPrompt?: string
  appendSystemPrompt?: string
  allowedTools?: string[]
  disallowedTools?: string[]
  mcpServers?: Record<string, McpServerConfigForProcessTransport>
  cwd?: string
  permissionMode?: PermissionMode
  abortController?: AbortController
  continueConversation?: boolean
  resumeConversation?: boolean
  sigwinch?: { rows: number; columns: number }
  outputFormat?: 'text' | 'json' | 'stream-json'
  betas?: SdkBeta[]
  enableRemoteControl?: boolean
  thinkingConfig?: ThinkingConfig
  jsonOutputSchema?: OutputFormat
  debug?: boolean
  verbose?: boolean
  effort?: EffortLevel
}

export type InternalOptions = Options & {
  querySource?: string
  parentSessionId?: string
  parentToolUseId?: string
  agentId?: string
  agentType?: string
}

// ============================================================================
// Query Types
// ============================================================================

export type Query = AsyncIterable<SDKMessage> & {
  abort(): void
  result: Promise<SDKResultMessage>
}

export type InternalQuery = AsyncIterable<SDKMessage> & {
  abort(): void
  result: Promise<SDKResultMessage>
}

// ============================================================================
// Session Types
// ============================================================================

export type SDKSessionOptions = {
  model?: string
  maxTurns?: number
  maxBudgetUsd?: number
  systemPrompt?: string
  appendSystemPrompt?: string
  allowedTools?: string[]
  disallowedTools?: string[]
  mcpServers?: Record<string, McpServerConfigForProcessTransport>
  cwd?: string
  permissionMode?: PermissionMode
  betas?: SdkBeta[]
  thinkingConfig?: ThinkingConfig
  effort?: EffortLevel
}

export type SDKSession = {
  sendMessage(
    message: string | AsyncIterable<SDKUserMessage>,
  ): AsyncIterable<SDKMessage> & { result: Promise<SDKResultMessage> }
  abort(): void
  sessionId: string
}

// ============================================================================
// Session Management Options
// ============================================================================

export type ListSessionsOptions = {
  dir?: string
  limit?: number
  offset?: number
}

export type GetSessionInfoOptions = {
  dir?: string
}

export type GetSessionMessagesOptions = {
  dir?: string
  limit?: number
  offset?: number
  includeSystemMessages?: boolean
}

export type SessionMutationOptions = {
  dir?: string
}

export type ForkSessionOptions = {
  dir?: string
  upToMessageId?: string
  title?: string
}

export type ForkSessionResult = {
  sessionId: string
}
