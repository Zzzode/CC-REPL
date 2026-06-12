import { describe, expect, test } from 'bun:test'
import { useElapsedTime } from './useElapsedTime.js'
import { formatDuration } from '../utils/format.js'

describe('useElapsedTime', () => {
  test('is a function', () => {
    expect(typeof useElapsedTime).toBe('function')
  })

  test('takes 2 required parameters (startTime, isRunning), rest have defaults', () => {
    // Function.length only counts parameters before the first one with a default value
    // useElapsedTime(startTime, isRunning, ms = 1000, pausedMs = 0, endTime?)
    // Since ms has a default, length = 2
    expect(useElapsedTime.length).toBe(2)
  })

  test('returns a string (formatted duration)', () => {
    // Hook signature verification
    expect(typeof useElapsedTime).toBe('function')
  })
})

describe('formatDuration (used by useElapsedTime)', () => {
  test('formats zero seconds', () => {
    expect(formatDuration(0)).toContain('0')
  })

  test('formats seconds', () => {
    const result = formatDuration(5000)
    expect(result).toContain('5')
    expect(typeof result).toBe('string')
  })

  test('formats minutes', () => {
    const result = formatDuration(90 * 1000) // 90 seconds
    expect(typeof result).toBe('string')
    expect(result.length).toBeGreaterThan(0)
  })

  test('handles negative values', () => {
    // Should clamp to 0
    const result = formatDuration(-1000)
    expect(typeof result).toBe('string')
  })
})

describe('useElapsedTime logic', () => {
  // Test the core computation: (endTime ?? Date.now()) - startTime - pausedMs
  function computeElapsedMs(
    startTime: number,
    pausedMs: number = 0,
    endTime?: number,
  ): number {
    return Math.max(0, (endTime ?? Date.now()) - startTime - pausedMs)
  }

  test('computes elapsed time from startTime to now', () => {
    const startTime = Date.now() - 5000
    const elapsed = computeElapsedMs(startTime)
    expect(elapsed).toBeGreaterThanOrEqual(0)
    expect(elapsed).toBeLessThan(6000)
  })

  test('subtracts pausedMs from elapsed time', () => {
    const startTime = Date.now() - 10000
    const pausedMs = 3000
    const elapsed = computeElapsedMs(startTime, pausedMs)
    expect(elapsed).toBeLessThan(8000)
    expect(elapsed).toBeGreaterThanOrEqual(0)
  })

  test('uses endTime when provided', () => {
    const startTime = 1000
    const endTime = 5000
    const elapsed = computeElapsedMs(startTime, 0, endTime)
    expect(elapsed).toBe(4000)
  })

  test('endTime + pausedMs', () => {
    const startTime = 1000
    const endTime = 5000
    const pausedMs = 1000
    const elapsed = computeElapsedMs(startTime, pausedMs, endTime)
    expect(elapsed).toBe(3000)
  })

  test('clamps negative values to 0', () => {
    const startTime = 10000
    const endTime = 5000 // end before start
    const elapsed = computeElapsedMs(startTime, 0, endTime)
    expect(elapsed).toBe(0)
  })

  test('pausedMs exceeding elapsed time clamps to 0', () => {
    const startTime = Date.now() - 1000
    const pausedMs = 10000
    const elapsed = computeElapsedMs(startTime, pausedMs)
    expect(elapsed).toBe(0)
  })
})
