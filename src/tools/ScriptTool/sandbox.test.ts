import { afterEach, beforeEach, describe, expect, mock, test } from 'bun:test'
import type { ToolUseContext } from '../../Tool.js'
import { createFileStateCacheWithSizeLimit } from '../../utils/fileStateCache.js'

type MockRunToolUse = (
  toolUse: { id: string; name: string; input: unknown },
  assistantMessage: unknown,
  canUseTool: unknown,
  context: ToolUseContext,
) => AsyncGenerator<{ message?: unknown }, void>

function createMinimalContext(tools: Array<{ name: string }> = []): ToolUseContext {
  return {
    options: { tools },
    messages: [],
    readFileState: createFileStateCacheWithSizeLimit(32),
    abortController: new AbortController(),
  } as unknown as ToolUseContext
}

function createToolResultMessage(
  toolUseID: string,
  toolUseResult: unknown,
  isError = false,
) {
  return {
    type: 'user',
    message: {
      role: 'user',
      content: [
        {
          type: 'tool_result',
          tool_use_id: toolUseID,
          content: isError ? 'error' : 'ok',
          is_error: isError,
        },
      ],
    },
    toolUseResult,
    uuid: `u-${toolUseID}`,
    timestamp: new Date().toISOString(),
  }
}

async function loadSandboxModule(runToolUse: MockRunToolUse) {
  mock.module('./primitiveTools.js', () => ({
    getScriptPrimitiveTools: () => [{ name: 'Read' }],
  }))

  mock.module('./typecheck.js', () => ({
    runScriptTypeCheck: async () => ({
      passed: true,
      durationMs: 0,
      errorCount: 0,
      warningCount: 0,
      totalDiagnosticCount: 0,
      diagnostics: [],
      truncated: false,
    }),
  }))

  mock.module('../../services/tools/toolExecution.js', () => ({
    runToolUse,
  }))

  return await import(`./sandbox.ts?test=${Date.now()}-${Math.random()}`)
}

describe('Script sandbox tool orchestration', () => {
  beforeEach(() => {
    mock.restore()
    mock.clearAllMocks()
  })

  afterEach(() => {
    mock.restore()
    mock.clearAllMocks()
  })

  test('uses runToolUse pipeline and exposes internal messages', async () => {
    let seenToolNames: string[] = []
    let seenInput: unknown

    const runToolUse: MockRunToolUse = async function* (
      toolUse,
      _assistantMessage,
      _canUseTool,
      context,
    ) {
      seenToolNames = (context.options.tools as Array<{ name: string }>).map(
        tool => tool.name,
      )
      seenInput = toolUse.input
      yield {
        message: createToolResultMessage(toolUse.id, {
          type: 'text',
          file: { content: 'hello from read' },
        }),
      }
    }

    const { executeInSandbox } = await loadSandboxModule(runToolUse)
    const context = createMinimalContext()
    const result = await executeInSandbox({
      code: "return await Read({ file_path: '/tmp/a.txt' })",
      context,
      canUseTool: async () => ({ behavior: 'allow' }),
      abortSignal: context.abortController.signal,
    })

    expect(result.result).toBe('hello from read')
    expect(seenInput).toEqual({ file_path: '/tmp/a.txt' })
    expect(seenToolNames).toContain('Read')
    expect(result.newMessages.length).toBe(2)
    expect(result.newMessages[0]?.type).toBe('assistant')
    expect(result.newMessages[1]?.type).toBe('user')
  })

  test('maps Read file_unchanged to cached content when available', async () => {
    const runToolUse: MockRunToolUse = async function* (toolUse) {
      yield {
        message: createToolResultMessage(toolUse.id, {
          type: 'file_unchanged',
          file: { filePath: '/tmp/cache-hit.txt' },
        }),
      }
    }

    const { executeInSandbox } = await loadSandboxModule(runToolUse)
    const context = createMinimalContext()
    context.readFileState.set('/tmp/cache-hit.txt', {
      content: 'cached content from readFileState',
      timestamp: Date.now(),
      offset: 0,
      limit: undefined,
    })

    const result = await executeInSandbox({
      code: "return await Read({ file_path: '/tmp/cache-hit.txt' })",
      context,
      canUseTool: async () => ({ behavior: 'allow' }),
      abortSignal: context.abortController.signal,
    })

    expect(result.result).toBe('cached content from readFileState')
  })

  test('maps Read file_unchanged to stable fallback message when cache misses', async () => {
    const runToolUse: MockRunToolUse = async function* (toolUse) {
      yield {
        message: createToolResultMessage(toolUse.id, {
          type: 'file_unchanged',
          file: { filePath: '/tmp/cache-miss.txt' },
        }),
      }
    }

    const { executeInSandbox } = await loadSandboxModule(runToolUse)
    const context = createMinimalContext()
    const result = await executeInSandbox({
      code: "return await Read({ file_path: '/tmp/cache-miss.txt' })",
      context,
      canUseTool: async () => ({ behavior: 'allow' }),
      abortSignal: context.abortController.signal,
    })

    expect(result.result).toBe('File unchanged since last read.')
  })

  test('turns tool_result errors into script execution errors', async () => {
    const runToolUse: MockRunToolUse = async function* (toolUse) {
      yield {
        message: createToolResultMessage(toolUse.id, 'permission denied', true),
      }
    }

    const { executeInSandbox } = await loadSandboxModule(runToolUse)
    const context = createMinimalContext()
    const result = await executeInSandbox({
      code: "return await Read({ file_path: '/tmp/error.txt' })",
      context,
      canUseTool: async () => ({ behavior: 'allow' }),
      abortSignal: context.abortController.signal,
    })

    expect(result.result).toBeUndefined()
    expect(result.stderr).toContain('permission denied')
    expect(result.newMessages.length).toBe(2)
    expect(result.newMessages[0]?.type).toBe('assistant')
    expect(result.newMessages[1]?.type).toBe('user')
  })
})
