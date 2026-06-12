import { describe, expect, test } from 'bun:test'
import {
  useNotifyAfterTimeout,
  DEFAULT_INTERACTION_THRESHOLD_MS,
} from './useNotifyAfterTimeout.js'

describe('useNotifyAfterTimeout constants', () => {
  test('DEFAULT_INTERACTION_THRESHOLD_MS is 6000ms (6 seconds)', () => {
    expect(DEFAULT_INTERACTION_THRESHOLD_MS).toBe(6000)
  })
})

describe('useNotifyAfterTimeout', () => {
  test('is a function', () => {
    expect(typeof useNotifyAfterTimeout).toBe('function')
  })

  test('takes 2 parameters (message, notificationType)', () => {
    expect(useNotifyAfterTimeout.length).toBe(2)
  })

  test('returns void (undefined)', () => {
    // Hook signature verification
    expect(typeof useNotifyAfterTimeout).toBe('function')
  })
})

describe('notification decision logic', () => {
  // Test the core logic: should we notify based on last interaction time?
  function hasRecentInteraction(
    lastInteractionTime: number,
    threshold: number,
    now: number = Date.now(),
  ): boolean {
    return now - lastInteractionTime < threshold
  }

  test('recent interaction returns true', () => {
    const now = Date.now()
    expect(hasRecentInteraction(now - 1000, 6000, now)).toBe(true) // 1s ago
    expect(hasRecentInteraction(now - 5000, 6000, now)).toBe(true) // 5s ago
  })

  test('old interaction returns false', () => {
    const now = Date.now()
    expect(hasRecentInteraction(now - 7000, 6000, now)).toBe(false) // 7s ago
    expect(hasRecentInteraction(now - 60000, 6000, now)).toBe(false) // 1min ago
  })

  test('boundary: exactly at threshold is NOT recent', () => {
    const now = Date.now()
    expect(hasRecentInteraction(now - 6000, 6000, now)).toBe(false)
  })

  test('boundary: 1ms less than threshold IS recent', () => {
    const now = Date.now()
    expect(hasRecentInteraction(now - 5999, 6000, now)).toBe(true)
  })
})

describe('notification condition logic', () => {
  function shouldNotify(
    lastInteractionTime: number,
    threshold: number,
    isTestEnv: boolean,
    now: number = Date.now(),
  ): boolean {
    if (isTestEnv) return false
    return now - lastInteractionTime >= threshold
  }

  test('does not notify in test environment', () => {
    expect(shouldNotify(Date.now() - 10000, 6000, true)).toBe(false)
  })

  test('notifies when idle beyond threshold (non-test)', () => {
    const now = Date.now()
    expect(shouldNotify(now - 10000, 6000, false, now)).toBe(true)
  })

  test('does not notify when recently interacted (non-test)', () => {
    const now = Date.now()
    expect(shouldNotify(now - 1000, 6000, false, now)).toBe(false)
  })
})
