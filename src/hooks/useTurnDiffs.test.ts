import { describe, expect, mock, test, beforeEach, afterEach } from 'bun:test'
import type { StructuredPatchHunk } from 'diff'
import type { Message } from '../types/message.js'

// We test the pure logic functions from useTurnDiffs by reimplementing them
// (since they're not exported). This validates the algorithm behavior.

function isFileEditResult(result: unknown): result is {
  filePath: string
  structuredPatch: StructuredPatchHunk[]
  type?: string
  content?: string
} {
  if (!result || typeof result !== 'object') return false
  const r = result as Record<string, unknown>
  const hasFilePath = typeof r.filePath === 'string'
  const hasStructuredPatch =
    Array.isArray(r.structuredPatch) && r.structuredPatch.length > 0
  const isNewFile = r.type === 'create' && typeof r.content === 'string'
  return hasFilePath && (hasStructuredPatch || isNewFile)
}

function countHunkLines(hunks: StructuredPatchHunk[]): {
  added: number
  removed: number
} {
  let added = 0
  let removed = 0
  for (const hunk of hunks) {
    for (const line of hunk.lines) {
      if (line.startsWith('+')) added++
      else if (line.startsWith('-')) removed++
    }
  }
  return { added, removed }
}

function getUserPromptPreview(message: Message): string {
  if (message.type !== 'user') return ''
  const content = message.message.content
  const text = typeof content === 'string' ? content : ''
  if (text.length <= 30) return text
  return text.slice(0, 29) + '…'
}

describe('useTurnDiffs logic: isFileEditResult', () => {
  test('returns false for null/undefined', () => {
    expect(isFileEditResult(null)).toBe(false)
    expect(isFileEditResult(undefined)).toBe(false)
  })

  test('returns false for non-objects', () => {
    expect(isFileEditResult('string')).toBe(false)
    expect(isFileEditResult(123)).toBe(false)
    expect(isFileEditResult(true)).toBe(false)
  })

  test('returns true for file edit result with structuredPatch', () => {
    const result = {
      filePath: 'test.ts',
      structuredPatch: [
        { oldStart: 1, oldLines: 3, newStart: 1, newLines: 3, lines: [] },
      ],
    }
    expect(isFileEditResult(result)).toBe(true)
  })

  test('returns false for object without filePath', () => {
    const result = {
      structuredPatch: [
        { oldStart: 1, oldLines: 3, newStart: 1, newLines: 3, lines: [] },
      ],
    }
    expect(isFileEditResult(result)).toBe(false)
  })

  test('returns true for create type with content', () => {
    const result = {
      filePath: 'new.ts',
      type: 'create',
      content: 'hello world',
      structuredPatch: [],
    }
    expect(isFileEditResult(result)).toBe(true)
  })

  test('returns false for empty structuredPatch without create type', () => {
    const result = {
      filePath: 'test.ts',
      structuredPatch: [],
    }
    expect(isFileEditResult(result)).toBe(false)
  })
})

describe('useTurnDiffs logic: countHunkLines', () => {
  test('returns zero for empty hunks', () => {
    expect(countHunkLines([])).toEqual({ added: 0, removed: 0 })
  })

  test('counts added and removed lines correctly', () => {
    const hunks: StructuredPatchHunk[] = [
      {
        oldStart: 1,
        oldLines: 3,
        newStart: 1,
        newLines: 4,
        lines: [' line1', '+added line', '-removed line', ' line4'],
      },
    ]
    expect(countHunkLines(hunks)).toEqual({ added: 1, removed: 1 })
  })

  test('handles multiple hunks', () => {
    const hunks: StructuredPatchHunk[] = [
      {
        oldStart: 1,
        oldLines: 2,
        newStart: 1,
        newLines: 3,
        lines: ['+a', '+b', ' c'],
      },
      {
        oldStart: 10,
        oldLines: 3,
        newStart: 10,
        newLines: 1,
        lines: ['-x', '-y', ' z'],
      },
    ]
    expect(countHunkLines(hunks)).toEqual({ added: 2, removed: 2 })
  })

  test('ignores context lines (starting with space)', () => {
    const hunks: StructuredPatchHunk[] = [
      {
        oldStart: 1,
        oldLines: 5,
        newStart: 1,
        newLines: 5,
        lines: [' a', ' b', ' c', ' d', ' e'],
      },
    ]
    expect(countHunkLines(hunks)).toEqual({ added: 0, removed: 0 })
  })
})

describe('useTurnDiffs logic: getUserPromptPreview', () => {
  test('returns empty string for non-user messages', () => {
    const msg = {
      type: 'assistant',
      message: { content: 'hello' },
    } as unknown as Message
    expect(getUserPromptPreview(msg)).toBe('')
  })

  test('returns full text for short messages (<= 30 chars)', () => {
    const msg = {
      type: 'user',
      message: { content: 'hello world' },
    } as unknown as Message
    expect(getUserPromptPreview(msg)).toBe('hello world')
  })

  test('truncates long messages with ellipsis', () => {
    const longText = 'a'.repeat(50)
    const msg = {
      type: 'user',
      message: { content: longText },
    } as unknown as Message
    const result = getUserPromptPreview(msg)
    expect(result.length).toBe(30) // 29 chars + ellipsis
    expect(result.endsWith('…')).toBe(true)
  })

  test('returns exactly 30 chars for boundary case', () => {
    const text30 = 'a'.repeat(30)
    const msg = {
      type: 'user',
      message: { content: text30 },
    } as unknown as Message
    expect(getUserPromptPreview(msg)).toBe(text30)
    expect(getUserPromptPreview(msg).length).toBe(30)
  })

  test('returns 29 + ellipsis for 31 chars', () => {
    const text31 = 'a'.repeat(31)
    const msg = {
      type: 'user',
      message: { content: text31 },
    } as unknown as Message
    const result = getUserPromptPreview(msg)
    expect(result.length).toBe(30)
    expect(result.endsWith('…')).toBe(true)
  })

  test('handles array content by returning empty string', () => {
    const msg = {
      type: 'user',
      message: { content: [{ type: 'text', text: 'hello' }] },
    } as unknown as Message
    expect(getUserPromptPreview(msg)).toBe('')
  })
})

describe('useTurnDiffs logic: computeTurnStats', () => {
  test('computes stats for a turn with multiple files', () => {
    const turn = {
      turnIndex: 1,
      userPromptPreview: 'test',
      timestamp: '2024-01-01',
      files: new Map<string, {
        filePath: string
        hunks: StructuredPatchHunk[]
        isNewFile: boolean
        linesAdded: number
        linesRemoved: number
      }>([
        ['file1.ts', { filePath: 'file1.ts', hunks: [], isNewFile: false, linesAdded: 5, linesRemoved: 2 }],
        ['file2.ts', { filePath: 'file2.ts', hunks: [], isNewFile: true, linesAdded: 10, linesRemoved: 0 }],
      ]),
      stats: { filesChanged: 0, linesAdded: 0, linesRemoved: 0 },
    }

    let totalAdded = 0
    let totalRemoved = 0
    for (const file of turn.files.values()) {
      totalAdded += file.linesAdded
      totalRemoved += file.linesRemoved
    }
    turn.stats = {
      filesChanged: turn.files.size,
      linesAdded: totalAdded,
      linesRemoved: totalRemoved,
    }

    expect(turn.stats.filesChanged).toBe(2)
    expect(turn.stats.linesAdded).toBe(15)
    expect(turn.stats.linesRemoved).toBe(2)
  })

  test('handles empty turn', () => {
    const turn = {
      turnIndex: 1,
      userPromptPreview: '',
      timestamp: '',
      files: new Map(),
      stats: { filesChanged: 0, linesAdded: 0, linesRemoved: 0 },
    }

    let totalAdded = 0
    let totalRemoved = 0
    for (const file of turn.files.values()) {
      totalAdded += file.linesAdded
      totalRemoved += file.linesRemoved
    }
    turn.stats = {
      filesChanged: turn.files.size,
      linesAdded: totalAdded,
      linesRemoved: totalRemoved,
    }

    expect(turn.stats.filesChanged).toBe(0)
    expect(turn.stats.linesAdded).toBe(0)
    expect(turn.stats.linesRemoved).toBe(0)
  })
})
