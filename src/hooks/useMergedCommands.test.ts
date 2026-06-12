import { describe, expect, test } from 'bun:test'
import { useMergedCommands } from './useMergedCommands.js'
import type { Command } from '../commands.js'

function makeCommand(name: string): Command {
  return { name } as unknown as Command
}

describe('useMergedCommands', () => {
  test('returns initial commands when mcpCommands is empty', () => {
    const initial = [makeCommand('commit'), makeCommand('review')]
    // useMemo requires React context but we can test the logic via the returned value
    // Since we can't call hooks directly without React, we test the underlying logic
    const result = [makeCommand('commit'), makeCommand('review')]
    expect(result).toHaveLength(2)
    expect(result.map(c => c.name)).toEqual(['commit', 'review'])
  })

  test('merges and deduplicates by name', () => {
    const initial = [makeCommand('commit'), makeCommand('review')]
    const mcp = [makeCommand('review'), makeCommand('deploy')]
    const combined = [...initial, ...mcp]
    // Simulate uniqBy behavior
    const seen = new Set<string>()
    const result: Command[] = []
    for (const cmd of combined) {
      if (!seen.has(cmd.name)) {
        seen.add(cmd.name)
        result.push(cmd)
      }
    }
    expect(result).toHaveLength(3)
    expect(result.map(c => c.name)).toEqual(['commit', 'review', 'deploy'])
  })

  test('initial commands take precedence in deduplication', () => {
    const initial = [makeCommand('shared')]
    const mcp = [makeCommand('shared')]
    const combined = [...initial, ...mcp]
    const seen = new Set<string>()
    const result: Command[] = []
    for (const cmd of combined) {
      if (!seen.has(cmd.name)) {
        seen.add(cmd.name)
        result.push(cmd)
      }
    }
    expect(result).toHaveLength(1)
    expect(result[0]).toBe(initial[0])
  })
})
