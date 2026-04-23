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
    code: z.string().describe('TypeScript/JavaScript code to execute'),
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
  searchHint: 'execute TypeScript in a pure-TS tool sandbox',
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

  // 保守地按可写处理：内部可能调用 Write/Edit，宿主层面不视为只读。
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

  // 权限交由底层被代理的工具自行校验（Read/Write/Edit 各有各的 checkPermissions）。
  // ScriptTool 自身是"编排器"而非具体副作用发起者，与 REPL 的定位一致。
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
    parentMessage: AssistantMessage,
  ) {
    const { code, timeout_ms } = input

    const result = await executeInSandbox({
      code,
      timeoutMs: timeout_ms ?? DEFAULT_TIMEOUT_MS,
      context,
      canUseTool,
      parentMessage,
      abortSignal: context.abortController.signal,
    })

    const output: ScriptToolOutput = {
      result: result.result,
      stdout: result.stdout,
      stderr: result.stderr,
      duration_ms: result.durationMs,
      timed_out: result.timedOut,
    }

    return { data: output }
  },

  mapToolResultToToolResultBlockParam(output, toolUseID) {
    // 对齐 BashTool 风格：stdout/errorMessage 用 filter(Boolean).join('\n') 拼接，
    // 错误信息统一用 <error>...</error> 包裹；执行失败场景走 is_error: true。
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
