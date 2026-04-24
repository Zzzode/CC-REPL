import { afterEach, beforeEach, describe, expect, mock, test } from 'bun:test'
import type { ToolUseContext } from '../../Tool.js'
import { DEFAULT_TIMEOUT_MS } from './constants.js'

function createMinimalContext(): ToolUseContext {
  return {
    abortController: new AbortController(),
  } as unknown as ToolUseContext
}

describe('ScriptTool call()', () => {
  beforeEach(() => {
    mock.restore()
    mock.clearAllMocks()
  })

  afterEach(() => {
    mock.restore()
    mock.clearAllMocks()
  })

  test('forwards executeInSandbox newMessages to ToolResult.newMessages', async () => {
    const forwardedMessages = [
      {
        type: 'assistant',
        message: { role: 'assistant', content: [] },
      },
      {
        type: 'user',
        message: { role: 'user', content: [] },
      },
    ]

    let seenTimeout: number | undefined
    let seenCode = ''

    mock.module('./sandbox.js', () => ({
      executeInSandbox: async (options: {
        code: string
        timeoutMs: number
      }) => {
        seenCode = options.code
        seenTimeout = options.timeoutMs
        return {
          result: { ok: true },
          stdout: 'stdout',
          stderr: '',
          durationMs: 12,
          timedOut: false,
          newMessages: forwardedMessages,
        }
      },
    }))

    const { ScriptTool } = await import(
      `./ScriptTool.ts?test=${Date.now()}-${Math.random()}`
    )

    const result = await ScriptTool.call(
      { code: 'return 1' },
      createMinimalContext(),
      async () => ({ behavior: 'allow' }),
      {} as never,
    )

    expect(seenCode).toBe('return 1')
    expect(seenTimeout).toBe(DEFAULT_TIMEOUT_MS)
    expect(result.data).toEqual({
      result: { ok: true },
      stdout: 'stdout',
      stderr: '',
      duration_ms: 12,
      timed_out: false,
    })
    expect(result.newMessages).toBe(forwardedMessages)
  })
})
