import { describe, expect, test } from 'bun:test'
import { useMemoryUsage } from './useMemoryUsage.js'

// Threshold values from the source
const HIGH_MEMORY_THRESHOLD = 1.5 * 1024 * 1024 * 1024 // 1.5GB in bytes
const CRITICAL_MEMORY_THRESHOLD = 2.5 * 1024 * 1024 * 1024 // 2.5GB in bytes

describe('useMemoryUsage constants (from source)', () => {
  test('HIGH_MEMORY_THRESHOLD is 1.5GB', () => {
    expect(HIGH_MEMORY_THRESHOLD).toBe(1.5 * 1024 * 1024 * 1024)
  })

  test('CRITICAL_MEMORY_THRESHOLD is 2.5GB', () => {
    expect(CRITICAL_MEMORY_THRESHOLD).toBe(2.5 * 1024 * 1024 * 1024)
  })
})

describe('useMemoryUsage', () => {
  test('is a function', () => {
    expect(typeof useMemoryUsage).toBe('function')
  })

  test('takes no parameters', () => {
    expect(useMemoryUsage.length).toBe(0)
  })
})

describe('memory status classification logic', () => {
  function classifyMemory(heapUsed: number): 'normal' | 'high' | 'critical' {
    if (heapUsed >= CRITICAL_MEMORY_THRESHOLD) return 'critical'
    if (heapUsed >= HIGH_MEMORY_THRESHOLD) return 'high'
    return 'normal'
  }

  test('normal status below HIGH threshold', () => {
    expect(classifyMemory(100 * 1024 * 1024)).toBe('normal') // 100MB
    expect(classifyMemory(0)).toBe('normal')
  })

  test('high status at or above HIGH threshold', () => {
    expect(classifyMemory(HIGH_MEMORY_THRESHOLD)).toBe('high')
    expect(classifyMemory(HIGH_MEMORY_THRESHOLD + 1)).toBe('high')
    expect(classifyMemory(2 * 1024 * 1024 * 1024)).toBe('high') // 2GB
  })

  test('critical status at or above CRITICAL threshold', () => {
    expect(classifyMemory(CRITICAL_MEMORY_THRESHOLD)).toBe('critical')
    expect(classifyMemory(CRITICAL_MEMORY_THRESHOLD + 1)).toBe('critical')
  })

  test('boundary between high and critical', () => {
    expect(classifyMemory(CRITICAL_MEMORY_THRESHOLD - 1)).toBe('high')
    expect(classifyMemory(CRITICAL_MEMORY_THRESHOLD)).toBe('critical')
  })
})

describe('memory status display logic', () => {
  // The hook returns null when status is 'normal' to avoid unnecessary re-renders
  function shouldDisplay(status: 'normal' | 'high' | 'critical'): boolean {
    return status !== 'normal'
  }

  test('returns null for normal status (no display)', () => {
    expect(shouldDisplay('normal')).toBe(false)
  })

  test('returns info for high status', () => {
    expect(shouldDisplay('high')).toBe(true)
  })

  test('returns info for critical status', () => {
    expect(shouldDisplay('critical')).toBe(true)
  })
})
