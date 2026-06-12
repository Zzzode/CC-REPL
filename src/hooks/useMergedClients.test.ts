import { describe, expect, test } from 'bun:test'
import { mergeClients, useMergedClients } from './useMergedClients.js'
import type { MCPServerConnection } from '../services/mcp/types.js'

function makeClient(name: string): MCPServerConnection {
  return { name } as unknown as MCPServerConnection
}

describe('mergeClients', () => {
  test('returns empty array when both are undefined', () => {
    expect(mergeClients(undefined, undefined)).toEqual([])
  })

  test('returns initial clients when mcpClients is undefined', () => {
    const initial = [makeClient('a'), makeClient('b')]
    expect(mergeClients(initial, undefined)).toEqual(initial)
  })

  test('returns initial clients when mcpClients is empty', () => {
    const initial = [makeClient('a')]
    expect(mergeClients(initial, [])).toEqual(initial)
  })

  test('returns empty array when initial is undefined and mcpClients is empty', () => {
    expect(mergeClients(undefined, [])).toEqual([])
  })

  test('merges and deduplicates by name', () => {
    const initial = [makeClient('a'), makeClient('b')]
    const mcp = [makeClient('b'), makeClient('c')]
    const result = mergeClients(initial, mcp)
    expect(result).toHaveLength(3)
    expect(result.map(c => c.name)).toEqual(['a', 'b', 'c'])
  })

  test('initial clients take precedence in deduplication', () => {
    const initial = [makeClient('shared')]
    const mcp = [makeClient('shared')]
    const result = mergeClients(initial, mcp)
    expect(result).toHaveLength(1)
    expect(result[0]).toBe(initial[0])
  })

  test('returns empty array when initial is undefined (mcpClients ignored without initial)', () => {
    // Current implementation: when initialClients is undefined, mcpClients are not included
    const mcp = [makeClient('a'), makeClient('b')]
    const result = mergeClients(undefined, mcp)
    expect(result).toEqual([])
  })

  test('returns initial clients when mcpClients is undefined', () => {
    const initial = [makeClient('a'), makeClient('b')]
    expect(mergeClients(initial, undefined)).toEqual(initial)
  })
})
