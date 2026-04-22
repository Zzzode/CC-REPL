import type { AssistantMessage } from '../../types/message.js'
import type { CanUseToolFn } from '../../hooks/useCanUseTool.js'
import { assertValidToolInput } from '../../services/tools/toolExecution.js'
import type { Tool, ToolUseContext } from '../../Tool.js'
import { getCwd } from '../../utils/cwd.js'
import { DEFAULT_TIMEOUT_MS, MAX_OUTPUT_SIZE } from './constants.js'
import { getScriptPrimitiveTools } from './primitiveTools.js'

export interface SandboxOptions {
  code: string
  timeoutMs?: number
  context: ToolUseContext
  canUseTool: CanUseToolFn
  parentMessage: AssistantMessage
  abortSignal?: AbortSignal
}

export interface SandboxResult {
  result: unknown
  stdout: string
  stderr: string
  durationMs: number
  error?: Error
  timedOut: boolean
}

// 懒加载 Bun.Transpiler，避免多次实例化的开销。
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
 * 为每个原始 Tool 构造代理函数，注入到脚本 VM 上下文中：
 *   `await Read({ file_path: '/tmp/a.txt' })` ⇢ 调用 FileReadTool.call()
 *
 * 设计背景：
 *   脚本里的 `await Read(...)` 直达 tool.call()，绕过了 QueryEngine 的
 *   runToolUse 派发链 —— 而派发链里那套 (abort / schema / validateInput)
 *   防御是 tool 正常运行依赖的前提。因此这里复用
 *   services/tools/toolExecution.ts#validateToolInput，
 *   与派发层使用同一份校验逻辑，避免出现"两套实现随时间漂移"。
 *
 * 代理函数执行顺序：
 *   1) abortSignal 检查：协作式终止（AsyncFunction 无法强制打断）。
 *   2) validateToolInput：schema + validateInput 二步校验。
 *   3) tool.call：宿主的权限、freshness、LSP 等能力完整保留。
 */
function buildToolProxies(
  tools: readonly Tool[],
  context: ToolUseContext,
  canUseTool: CanUseToolFn,
  parentMessage: AssistantMessage,
  abortSignal?: AbortSignal,
): Record<string, (input: unknown) => Promise<unknown>> {
  const proxies: Record<string, (input: unknown) => Promise<unknown>> = {}

  for (const tool of tools) {
    proxies[tool.name] = async (input: unknown): Promise<unknown> => {
      // 1) 协作式 abort：每次 tool 调用前检查，尽早让脚本 await 抛错。
      if (abortSignal?.aborted) {
        throw new Error('Script execution was aborted')
      }

      // 2) 共享校验：与派发层同源（schema + validateInput），
      //    错误消息用派发层同款 formatZodValidationError 渲染。
      const validatedInput = await assertValidToolInput(tool, input, context)

      const result = await tool.call(
        validatedInput as never,
        context,
        canUseTool,
        parentMessage,
      )
      return result.data
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
    parentMessage,
    abortSignal,
  } = options

  const start = Date.now()
  let stdout = ''
  let stderr = ''
  let timedOut = false

  // 构造 console 代理：只把输出收集到 stdout/stderr，不泄漏到真实 console。
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

  // utils 命名空间：无副作用、无权限的小工具。
  // 刻意保持克制——不暴露 env / exec / 任何 I/O；这里只放纯函数语义的东西。
  const sandboxUtils = Object.freeze({
    // 当前工作目录（只读，避免用户代码 shadow）。
    get cwd(): string {
      return getCwd()
    },
    // 可被外层 abortSignal 中断的 sleep —— 关键：超时/取消场景下不会阻塞到期。
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

  // 装配 VM 上下文：工具代理 + console + utils。
  const primitiveTools = getScriptPrimitiveTools()
  const toolProxies = buildToolProxies(
    primitiveTools,
    context,
    canUseTool,
    parentMessage,
    abortSignal,
  )

  const sandboxContext = {
    ...toolProxies,
    console: sandboxConsole,
    utils: sandboxUtils,
  }

  // TS → JS 转译（语法错误提前兜底，避免 AsyncFunction 抛栈污染）。
  let transpiled: string
  try {
    transpiled = getTranspiler().transformSync(code)
  } catch (syntaxError) {
    return {
      result: undefined,
      stdout: '',
      stderr: `Syntax error: ${syntaxError instanceof Error ? syntaxError.message : String(syntaxError)}`,
      durationMs: Date.now() - start,
      error: syntaxError instanceof Error ? syntaxError : new Error(String(syntaxError)),
      timedOut: false,
    }
  }

  // 把工具代理按名称逐个解构到局部作用域，让用户代码可以直接 `await Read(...)`。
  const toolNames = Object.keys(toolProxies)
  const bindingsPrelude = toolNames
    .map(name => `const ${name} = __ctx[${JSON.stringify(name)}];`)
    .join('\n')
  const wrappedCode = `
${bindingsPrelude}
const console = __ctx.console;
const utils = __ctx.utils;
${transpiled}
`

  // 超时 + 取消控制。任一路径触发即 reject；Promise.race 把正常执行撞下。
  let timer: ReturnType<typeof setTimeout> | undefined
  const timeoutPromise = new Promise<never>((_, reject) => {
    timer = setTimeout(() => {
      timedOut = true
      reject(new Error(`Script execution timed out after ${timeoutMs / 1000}s`))
    }, timeoutMs)
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
    }
  } catch (error) {
    if (timer) clearTimeout(timer)
    return {
      result: undefined,
      stdout: truncateOutput(stdout),
      stderr:
        truncateOutput(stderr) +
        (error instanceof Error ? '\n' + error.message : String(error)),
      durationMs: Date.now() - start,
      error: error instanceof Error ? error : new Error(String(error)),
      timedOut,
    }
  }
}

// ---------- 工具函数 ----------

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
