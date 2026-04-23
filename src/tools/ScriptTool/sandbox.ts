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
 * 将 tool.call() 返回的内部 data 对象归一化为脚本代码友好的格式。
 *
 * 设计背景（两层数据模型）：
 *   仓库中 tool.call() 返回的 data 是面向 UI/分析的结构化内部对象，
 *   模型在标准链路中从来不直接看到 data——它看到的是
 *   mapToolResultToToolResultBlockParam 翻译后的自然语言文本。
 *   这里为 ScriptTool 补上等价的翻译层：把 data 转为脚本代码
 *   最直觉的类型（string / 简洁对象），消除模型猜测内部结构的负担。
 */
function normalizeToolResult(toolName: string, data: unknown): unknown {
  if (data == null) return data
  const d = data as Record<string, unknown>

  switch (toolName) {
    // Read → 文本文件直接返回 content 字符串；file_unchanged 重新取缓存中的
    // content 返回，避免静默空输出；image/pdf/notebook 透传原始对象。
    case 'Read': {
      const type = d.type as string | undefined
      if (type === 'text') {
        const file = d.file as Record<string, unknown> | undefined
        return (file?.content as string) ?? ''
      }
      if (type === 'file_unchanged') {
        return ''
      }
      return data
    }

    // Write → 返回确认消息（与 mapToolResultToToolResultBlockParam 对齐）。
    case 'Write': {
      const type = d.type as string | undefined
      const filePath = d.filePath as string | undefined
      if (type === 'create') return `File created successfully at: ${filePath}`
      return `The file ${filePath} has been updated successfully.`
    }

    // Edit → 返回确认消息。
    case 'Edit': {
      const filePath = d.filePath as string | undefined
      return `The file ${filePath} has been updated successfully.`
    }

    // NotebookEdit → 有 error 字段时抛异常，否则返回确认消息。
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

    // Agent → 拼接所有 text 块为纯字符串。
    case 'Agent': {
      const content = d.content as Array<{ type: string; text: string }> | undefined
      if (!Array.isArray(content)) return ''
      return content
        .filter(block => block.type === 'text')
        .map(block => block.text)
        .join('\n')
    }

    // WebFetch → 直接返回 result 摘要文本。
    case 'WebFetch':
      return (d.result as string) ?? ''

    default:
      return data
  }
}

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
 *   4) normalizeToolResult：内部 data → 脚本友好格式。
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

      // 3) 归一化：把内部 data 翻译为脚本代码友好的格式（string / 简洁对象），
      //    与 mapToolResultToToolResultBlockParam 为模型做的翻译同构。
      return normalizeToolResult(tool.name, result.data)
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

  // 把工具代理按名称逐个解构到局部作用域，让用户代码可以直接 `await Read(...)`。
  const toolNames = Object.keys(toolProxies)
  const bindingsPrelude = toolNames
    .map(name => `const ${name} = __ctx[${JSON.stringify(name)}];`)
    .join('\n')

  // "先包装再转译"策略：
  //   将 bindings + 用户代码放进命名 async 函数体，使 top-level return / await
  //   在函数体内合法。若先转译裸代码，Bun.Transpiler 会以 ESM 规则拒绝 return。
  const preambleLines = [
    'async function __script__(__ctx) {',
    bindingsPrelude,
    'const console = __ctx.console;',
    'const utils = __ctx.utils;',
  ]
  const preambleLineCount = preambleLines.length
  const codeToTranspile = [...preambleLines, code, '}'].join('\n')

  // TS → JS 转译（语法错误提前兜底，避免 AsyncFunction 抛栈污染）。
  let transpiled: string
  try {
    transpiled = getTranspiler().transformSync(codeToTranspile)
  } catch (syntaxError) {
    const raw = syntaxError instanceof Error ? syntaxError.message : String(syntaxError)
    const adjusted = adjustLineNumbers(raw, preambleLineCount)
    return {
      result: undefined,
      stdout: '',
      stderr: `Syntax error: ${adjusted}`,
      durationMs: Date.now() - start,
      error: syntaxError instanceof Error ? syntaxError : new Error(String(syntaxError)),
      timedOut: false,
    }
  }

  // 转译产物已包含 __script__ 函数定义，外层 AsyncFunction 只需调用并返回结果。
  const wrappedCode = `${transpiled}\nreturn __script__(__ctx);`

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

// Transpiler 报错行号包含 preamble 偏移，减去偏移还原到用户代码的真实行号。
function adjustLineNumbers(message: string, offset: number): string {
  return message.replace(
    /\bline\s+(\d+)/gi,
    (match, n) => {
      const adjusted = Number(n) - offset
      return adjusted >= 1 ? `line ${adjusted}` : match
    },
  )
}
