import { z } from 'zod/v4'
import type { CanUseToolFn } from '../../hooks/useCanUseTool.js'
import type { AssistantMessage } from '../../types/message.js'
import type { ValidationResult, ToolUseContext } from '../../Tool.js'
import { buildTool, type ToolDef } from '../../Tool.js'
import { getCwd } from '../../utils/cwd.js'
import { lazySchema } from '../../utils/lazySchema.js'
import type { PermissionDecision } from '../../utils/permissions/PermissionResult.js'
import { DEFAULT_TIMEOUT_MS, MAX_TIMEOUT_MS, SCRIPT_TOOL_NAME, SCRIPT_VALIDATION_ERROR_CODE } from './constants.js'
import { DESCRIPTION, getPrompt } from './prompt.js'
import { executeInSandbox } from './sandbox.js'
import type { ScriptToolOutput } from './types.js'
import {
  userFacingName,
  renderToolUseMessage,
  renderToolResultMessage,
  renderToolUseErrorMessage,
  getToolUseSummary,
} from './UI.js'

const inputSchema = lazySchema(() =>
  z.strictObject({
    code: z
      .string()
      .describe(
        'TypeScript code to execute (JavaScript is supported as a TypeScript subset)',
      ),
    description: z
      .string()
      .optional()
      .describe('Brief description of what the script does'),
    timeout_ms: z
      .number()
      .int()
      .min(1000)
      .max(MAX_TIMEOUT_MS)
      .optional()
      .describe('Timeout in milliseconds'),
  }),
)
type InputSchema = ReturnType<typeof inputSchema>

export const ScriptTool = buildTool({
  name: SCRIPT_TOOL_NAME,
  searchHint: 'execute type-checked TypeScript in a pure-TS tool sandbox',
  maxResultSizeChars: 100_000,

  async description() {
    return DESCRIPTION
  },

  userFacingName,

  getToolUseSummary,

  getActivityDescription(input) {
    return input.description ?? 'Executing script'
  },

  get inputSchema(): InputSchema {
    return inputSchema()
  },

  isConcurrencySafe() {
    return false
  },

  // Conservatively treat as writable: it may call Write/Edit internally, so host level is not read-only.
  isReadOnly() {
    return false
  },

  getPath(): string {
    return getCwd()
  },

  async validateInput({ code, timeout_ms }): Promise<ValidationResult> {
    if (!code.trim()) {
      return {
        result: false,
        message: 'Code cannot be empty',
        errorCode: SCRIPT_VALIDATION_ERROR_CODE.EMPTY_CODE,
      }
    }
    if (timeout_ms && timeout_ms > MAX_TIMEOUT_MS) {
      return {
        result: false,
        message: `Timeout cannot exceed ${MAX_TIMEOUT_MS / 1000}s`,
        errorCode: SCRIPT_VALIDATION_ERROR_CODE.TIMEOUT_EXCEEDED,
      }
    }
    return { result: true }
  },

  // Permissions are validated by each proxied tool itself (Read/Write/Edit each has its own checkPermissions).
  // ScriptTool itself is an orchestrator, not the direct side-effect initiator, aligned with REPL positioning.
  async checkPermissions(): Promise<PermissionDecision> {
    return { behavior: 'allow', decisionReason: { type: 'rule' } }
  },

  async prompt() {
    return getPrompt()
  },

  renderToolUseMessage,
  renderToolResultMessage,
  renderToolUseErrorMessage,

  async call(
    input,
    context: ToolUseContext,
    canUseTool: CanUseToolFn,
    _parentMessage: AssistantMessage,
  ) {
    const { code, timeout_ms } = input

    const result = await executeInSandbox({
      code,
      timeoutMs: timeout_ms ?? DEFAULT_TIMEOUT_MS,
      context,
      canUseTool,
      abortSignal: context.abortController.signal,
    })

    const output: ScriptToolOutput = {
      result: result.result,
      stdout: result.stdout,
      stderr: result.stderr,
      duration_ms: result.durationMs,
      timed_out: result.timedOut,
    }

    return { data: output, newMessages: result.newMessages }
  },

  mapToolResultToToolResultBlockParam(output, toolUseID) {
    // Match BashTool style: concatenate stdout/errorMessage with filter(Boolean).join('\n'),
    // wrap error messages uniformly with <error>...</error>; execution-failure paths use is_error: true.
    const resultText =
      output.result === undefined || output.result === null
        ? ''
        : typeof output.result === 'string'
          ? output.result
          : JSON.stringify(output.result, null, 2)

    const stdout = output.stdout.trimEnd()
    const stderr = output.stderr.trim()

    let errorMessage = stderr
    if (output.timed_out) {
      if (errorMessage) errorMessage += '\n'
      errorMessage += '<error>Script execution timed out</error>'
    }
    const isExecutionError = output.timed_out || (output.result === undefined && stderr.length > 0)

    return {
      tool_use_id: toolUseID,
      type: 'tool_result',
      content: [resultText, stdout, errorMessage].filter(Boolean).join('\n'),
      is_error: isExecutionError,
    }
  },
} satisfies ToolDef<InputSchema, ScriptToolOutput>)
