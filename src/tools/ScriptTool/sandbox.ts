import { randomUUID } from 'crypto'
import type { ToolUseBlock } from '@anthropic-ai/sdk/resources/index.mjs'
import type { CanUseToolFn } from '../../hooks/useCanUseTool.js'
import { runToolUse } from '../../services/tools/toolExecution.js'
import type { Tool, ToolUseContext } from '../../Tool.js'
import { getCwd } from '../../utils/cwd.js'
import { createAssistantMessage } from '../../utils/messages.js'
import { expandPath } from '../../utils/path.js'
import type { Message } from '../../types/message.js'
import {
  DEFAULT_TIMEOUT_MS,
  MAX_OUTPUT_SIZE,
  MAX_TYPECHECK_DIAGNOSTICS,
} from './constants.js'
import { formatSyntaxError, formatTypeCheckFailure } from './formatDiagnostics.js'
import { getScriptPrimitiveTools } from './primitiveTools.js'
import { runScriptTypeCheck } from './typecheck.js'

export interface SandboxOptions {
  code: string
  timeoutMs?: number
  context: ToolUseContext
  canUseTool: CanUseToolFn
  abortSignal?: AbortSignal
}

export interface SandboxResult {
  result: unknown
  stdout: string
  stderr: string
  durationMs: number
  error?: Error
  timedOut: boolean
  newMessages: ScriptVisibleMessage[]
}

type ScriptVisibleMessage = Extract<
  Message,
  { type: 'assistant' | 'user' | 'attachment' | 'system' }
>

type ScriptRuntime = {
  context: ToolUseContext
  tools: readonly Tool[]
  visibleMessages: ScriptVisibleMessage[]
}

// Lazily load Bun.Transpiler to avoid repeated instantiation overhead.
let transpiler: Bun.Transpiler | null = null
function getTranspiler(): Bun.Transpiler {
  if (!transpiler) {
    transpiler = new Bun.Transpiler({
      loader: 'ts',
      target: 'bun',
      define: {},
    })
  }
  return transpiler
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor

/**
 * Normalize the internal data object returned by tool.call() into script-friendly formats.
 *
 * Design background (two-layer data model):
 *   In this repo, data from tool.call() is a structured internal object for UI/analytics,
 *   and models never directly see data in the standard flow—they see
 *   natural-language text translated by mapToolResultToToolResultBlockParam.
 *   Here ScriptTool adds an equivalent translation layer: convert data into
 *   the most intuitive types (string / concise object), reducing model guesswork about internal structure.
 */
function normalizeToolResult(
  toolName: string,
  data: unknown,
  context: ToolUseContext,
): unknown {
  if (data == null) return data
  const d = data as Record<string, unknown>

  switch (toolName) {
    // Read -> return content string directly for text files; for file_unchanged, read cached
    // content to avoid silent empty output; pass through raw objects for image/pdf/notebook.
    case 'Read': {
      const type = d.type as string | undefined
      if (type === 'text') {
        const file = d.file as Record<string, unknown> | undefined
        return (file?.content as string) ?? ''
      }
      if (type === 'file_unchanged') {
        const file = d.file as Record<string, unknown> | undefined
        const filePath = file?.filePath
        if (typeof filePath === 'string' && filePath.length > 0) {
          const cached = context.readFileState.get(expandPath(filePath))
          if (cached?.content !== undefined) {
            return cached.content
          }
        }
        return 'File unchanged since last read.'
      }
      return data
    }

    // Write -> return confirmation message (aligned with mapToolResultToToolResultBlockParam).
    case 'Write': {
      const type = d.type as string | undefined
      const filePath = d.filePath as string | undefined
      if (type === 'create') return `File created successfully at: ${filePath}`
      return `The file ${filePath} has been updated successfully.`
    }

    // Edit -> return confirmation message.
    case 'Edit': {
      const filePath = d.filePath as string | undefined
      return `The file ${filePath} has been updated successfully.`
    }

    // NotebookEdit -> throw when error field exists; otherwise return confirmation message.
    case 'NotebookEdit': {
      const error = d.error as string | undefined
      if (error) throw new Error(error)
      const cellId = d.cell_id as string | undefined
      const editMode = (d.edit_mode as string) ?? 'replace'
      switch (editMode) {
        case 'insert':
          return `Inserted cell ${cellId}`
        case 'delete':
          return `Deleted cell ${cellId}`
        default:
          return `Updated cell ${cellId}`
      }
    }

    // Agent -> concatenate all text blocks into a plain string.
    case 'Agent': {
      const content = d.content as Array<{ type: string; text: string }> | undefined
      if (!Array.isArray(content)) return ''
      return content
        .filter(block => block.type === 'text')
        .map(block => block.text)
        .join('\n')
    }

    // WebFetch -> return result summary text directly.
    case 'WebFetch':
      return (d.result as string) ?? ''

    default:
      return data
  }
}

/**
 * Build a proxy function for each primitive tool and inject it into the script VM context:
 *   `await Read({ file_path: '/tmp/a.txt' })` -> follows the standard runToolUse execution path
 *
 * Design background:
 *   ScriptTool must stay consistent with regular tool calls (permissions, hooks, tool_result,
 *   analytics), so internal calls no longer bypass tool.call(); they directly reuse
 *   runToolUse。
 *
 * Proxy function execution order:
 *   1) abortSignal check: cooperative termination (AsyncFunction cannot be forcibly interrupted).
 *   2) Construct a synthetic tool_use assistant message and execute runToolUse.
 *   3) Extract toolUseResult from the matching tool_result; throw on error.
 *   4) normalizeToolResult: toolUseResult -> script-friendly format.
 */
function isScriptVisibleMessage(message: Message): message is ScriptVisibleMessage {
  return (
    message.type === 'assistant' ||
    message.type === 'user' ||
    message.type === 'attachment' ||
    message.type === 'system'
  )
}

function mergeInternalTools(contextTools: readonly Tool[]): readonly Tool[] {
  const merged = [...contextTools]
  const seen = new Set(merged.map(tool => tool.name))
  for (const primitive of getScriptPrimitiveTools()) {
    if (!seen.has(primitive.name)) {
      merged.push(primitive)
      seen.add(primitive.name)
    }
  }
  return merged
}

function appendRuntimeMessage(
  runtime: ScriptRuntime,
  message: Message,
): void {
  runtime.context.messages.push(message)
  if (isScriptVisibleMessage(message)) {
    runtime.visibleMessages.push(message)
  }
}

function getToolResultStatus(
  message: Message,
  toolUseID: string,
): { isError: boolean } | null {
  if (message.type !== 'user' || !Array.isArray(message.message.content)) {
    return null
  }
  for (const block of message.message.content) {
    if (block.type === 'tool_result' && block.tool_use_id === toolUseID) {
      return { isError: block.is_error === true }
    }
  }
  return null
}

function buildToolErrorMessage(toolName: string, toolResult: unknown): string {
  if (typeof toolResult === 'string' && toolResult.trim()) {
    return toolResult
  }
  if (toolResult instanceof Error && toolResult.message) {
    return toolResult.message
  }
  if (toolResult !== undefined) {
    try {
      return JSON.stringify(toolResult)
    } catch {
      return String(toolResult)
    }
  }
  return `${toolName} failed`
}

function buildToolProxies(
  tools: readonly Tool[],
  runtime: ScriptRuntime,
  canUseTool: CanUseToolFn,
  abortSignal?: AbortSignal,
): Record<string, (input: unknown) => Promise<unknown>> {
  const proxies: Record<string, (input: unknown) => Promise<unknown>> = {}

  for (const tool of tools) {
    proxies[tool.name] = async (input: unknown): Promise<unknown> => {
      // 1) Cooperative abort: check before each tool call so script await fails early.
      if (abortSignal?.aborted) {
        throw new Error('Script execution was aborted')
      }

      // 2) Use the standard runToolUse path: fully reuse permissions/validation/hooks/tool_result.
      const toolUseID = `script-${randomUUID()}`
      const toolUse: ToolUseBlock = {
        type: 'tool_use',
        id: toolUseID,
        name: tool.name,
        input: (input as Record<string, unknown>) ?? {},
      }
      const assistantMessage = createAssistantMessage({ content: [toolUse] })
      appendRuntimeMessage(runtime, assistantMessage)

      let hasResult = false
      let isError = false
      let toolResult: unknown

      for await (const update of runToolUse(
        toolUse,
        assistantMessage,
        canUseTool,
        runtime.context,
      )) {
        if (update.contextModifier) {
          const nextContext = update.contextModifier.modifyContext(runtime.context)
          runtime.context = {
            ...nextContext,
            options: {
              ...nextContext.options,
              tools: runtime.tools,
            },
            messages: runtime.context.messages,
          }
        }

        if (!update.message) continue
        appendRuntimeMessage(runtime, update.message)
        const status = getToolResultStatus(update.message, toolUseID)
        if (status) {
          hasResult = true
          isError = status.isError
          toolResult = update.message.toolUseResult
        }
      }

      if (!hasResult) {
        throw new Error(
          `Internal Script tool call (${tool.name}) finished without a tool_result`,
        )
      }
      if (isError) {
        throw new Error(buildToolErrorMessage(tool.name, toolResult))
      }

      // 3) Normalize: toolUseResult -> script-friendly value (string / concise object).
      return normalizeToolResult(tool.name, toolResult, runtime.context)
    }
  }

  return proxies
}

export async function executeInSandbox(options: SandboxOptions): Promise<SandboxResult> {
  const {
    code,
    timeoutMs = DEFAULT_TIMEOUT_MS,
    context,
    canUseTool,
    abortSignal,
  } = options

  const start = Date.now()
  let stdout = ''
  let stderr = ''
  let timedOut = false
  const runtimeContextTools = mergeInternalTools(context.options.tools)
  const runtime: ScriptRuntime = {
    context: {
      ...context,
      options: {
        ...context.options,
        tools: runtimeContextTools,
      },
      messages: [...context.messages],
    },
    tools: runtimeContextTools,
    visibleMessages: [],
  }

  // Build a console proxy: collect output only to stdout/stderr, without leaking to the real console.
  const sandboxConsole = {
    log: (...args: unknown[]) => {
      stdout += args.map(formatConsoleArg).join(' ') + '\n'
    },
    error: (...args: unknown[]) => {
      stderr += args.map(formatConsoleArg).join(' ') + '\n'
    },
    warn: (...args: unknown[]) => {
      stderr += args.map(formatConsoleArg).join(' ') + '\n'
    },
    info: (...args: unknown[]) => {
      stdout += args.map(formatConsoleArg).join(' ') + '\n'
    },
  }

  // utils namespace: side-effect-free, permission-free helpers.
  // Intentionally minimal: do not expose env / exec / any I/O; only pure-function semantics belong here.
  const sandboxUtils = Object.freeze({
    // Current working directory (read-only, avoids shadowing by user code).
    get cwd(): string {
      return getCwd()
    },
    // Sleep interruptible by outer abortSignal — key for timeout/cancel paths to avoid blocking until expiry.
    sleep(ms: number): Promise<void> {
      return new Promise<void>((resolve, reject) => {
        if (abortSignal?.aborted) {
          reject(new Error('Script execution was aborted'))
          return
        }
        const timer = setTimeout(() => {
          abortSignal?.removeEventListener('abort', onAbort)
          resolve()
        }, Math.max(0, ms))
        const onAbort = () => {
          clearTimeout(timer)
          reject(new Error('Script execution was aborted'))
        }
        abortSignal?.addEventListener('abort', onAbort, { once: true })
      })
    },
  })

  // Assemble VM context: tool proxies + console + utils.
  const primitiveTools = getScriptPrimitiveTools()
  const toolProxies = buildToolProxies(
    primitiveTools,
    runtime,
    canUseTool,
    abortSignal,
  )

  const sandboxContext = {
    ...toolProxies,
    console: sandboxConsole,
    utils: sandboxUtils,
  }

  // Destructure tool proxies by name into local scope so user code can call `await Read(...)` directly.
  const toolNames = Object.keys(toolProxies)
  const bindingsPrelude = toolNames
    .map(name => `const ${name} = __ctx[${JSON.stringify(name)}];`)
    .join('\n')

  // "Wrap-then-transpile" strategy:
  //   put bindings + user code into a named async function body so top-level return / await
  //   are valid inside the function body. If bare code is transpiled first, Bun.Transpiler rejects return under ESM rules.
  const preambleLines = [
    'async function __script__(__ctx) {',
    bindingsPrelude,
    'const console = __ctx.console;',
    'const utils = __ctx.utils;',
  ]
  const codeToTranspile = [...preambleLines, code, '}'].join('\n')
  const preambleLineCount =
    codeToTranspile.split('\n').length - code.split('\n').length - 1

  // TS -> JS transpilation (catches syntax errors early, avoiding AsyncFunction stack-noise).
  let transpiled: string
  try {
    transpiled = getTranspiler().transformSync(codeToTranspile)
  } catch (syntaxError) {
    return {
      result: undefined,
      stdout: '',
      stderr: formatSyntaxError(syntaxError, preambleLineCount),
      durationMs: Date.now() - start,
      error:
        syntaxError instanceof Error
          ? syntaxError
          : new Error(String(syntaxError)),
      timedOut: false,
      newMessages: runtime.visibleMessages,
    }
  }

  if (abortSignal?.aborted) {
    return {
      result: undefined,
      stdout: truncateOutput(stdout),
      stderr: [truncateOutput(stderr), 'Script execution was aborted']
        .filter(Boolean)
        .join('\n'),
      durationMs: Date.now() - start,
      error: new Error('Script execution was aborted'),
      timedOut: false,
      newMessages: runtime.visibleMessages,
    }
  }

  // Enforce TypeScript semantic checks: do not enter execution when checks fail.
  const typeCheckResult = await runScriptTypeCheck({
    code: codeToTranspile,
    preambleLineCount,
    maxDiagnostics: MAX_TYPECHECK_DIAGNOSTICS,
  })

  if (!typeCheckResult.passed) {
    return {
      result: undefined,
      stdout: truncateOutput(stdout),
      stderr: [truncateOutput(stderr), formatTypeCheckFailure(typeCheckResult)]
        .filter(Boolean)
        .join('\n'),
      durationMs: Date.now() - start,
      error: new Error('Script type check failed'),
      timedOut: false,
      newMessages: runtime.visibleMessages,
    }
  }

  const elapsedMs = Date.now() - start
  const remainingTimeoutMs = timeoutMs - elapsedMs
  if (remainingTimeoutMs <= 0) {
    return {
      result: undefined,
      stdout: truncateOutput(stdout),
      stderr: [
        truncateOutput(stderr),
        `Script execution timed out after ${timeoutMs / 1000}s`,
      ]
        .filter(Boolean)
        .join('\n'),
      durationMs: elapsedMs,
      error: new Error(`Script execution timed out after ${timeoutMs / 1000}s`),
      timedOut: true,
      newMessages: runtime.visibleMessages,
    }
  }

  // Transpiled output already contains __script__ definition; outer AsyncFunction only needs to call and return it.
  const wrappedCode = `${transpiled}\nreturn __script__(__ctx);`

  // Timeout + cancellation control. Any trigger rejects; Promise.race interrupts normal execution.
  let timer: ReturnType<typeof setTimeout> | undefined
  const timeoutPromise = new Promise<never>((_, reject) => {
    timer = setTimeout(() => {
      timedOut = true
      reject(new Error(`Script execution timed out after ${timeoutMs / 1000}s`))
    }, remainingTimeoutMs)
    if (abortSignal) {
      abortSignal.addEventListener(
        'abort',
        () => {
          if (timer) clearTimeout(timer)
          reject(new Error('Script execution was aborted'))
        },
        { once: true },
      )
    }
  })

  try {
    const fn = new AsyncFunction('__ctx', wrappedCode)
    const result = await Promise.race([fn(sandboxContext), timeoutPromise])
    if (timer) clearTimeout(timer)
    return {
      result,
      stdout: truncateOutput(stdout),
      stderr: truncateOutput(stderr),
      durationMs: Date.now() - start,
      timedOut,
      newMessages: runtime.visibleMessages,
    }
  } catch (error) {
    if (timer) clearTimeout(timer)
    return {
      result: undefined,
      stdout: truncateOutput(stdout),
      stderr:
        [truncateOutput(stderr), error instanceof Error ? error.message : String(error)]
          .filter(Boolean)
          .join('\n'),
      durationMs: Date.now() - start,
      error: error instanceof Error ? error : new Error(String(error)),
      timedOut,
      newMessages: runtime.visibleMessages,
    }
  }
}

// ---------- Utility helpers ----------

function formatConsoleArg(arg: unknown): string {
  if (typeof arg === 'string') return arg
  try {
    return JSON.stringify(arg)
  } catch {
    return String(arg)
  }
}

function truncateOutput(output: string): string {
  if (output.length <= MAX_OUTPUT_SIZE) return output
  const half = Math.floor(MAX_OUTPUT_SIZE / 2)
  return output.slice(0, half) + '\n... [truncated] ...\n' + output.slice(-half)
}
