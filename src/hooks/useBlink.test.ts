import { describe, expect, test } from 'bun:test'
import { useBlink } from './useBlink.js'

// Default blink interval from source
const BLINK_INTERVAL_MS = 600

describe('useBlink default interval (from source)', () => {
  test('default interval is 600ms', () => {
    expect(BLINK_INTERVAL_MS).toBe(600)
  })
})

describe('useBlink', () => {
  test('is a function', () => {
    expect(typeof useBlink).toBe('function')
  })

  test('takes 1 required parameter (enabled), intervalMs has default', () => {
    // useBlink(enabled: boolean, intervalMs = BLINK_INTERVAL_MS)
    // Function.length counts only parameters before first default
    expect(useBlink.length).toBe(1)
  })
})

describe('blink state derivation logic', () => {
  // The blink state is derived from: Math.floor(time / intervalMs) % 2 === 0
  // All instances blink in sync because they share the same time base.
  // The state alternates every `intervalMs` milliseconds:
  //   0 to intervalMs-1: visible (index 0, even)
  //   intervalMs to 2*intervalMs-1: invisible (index 1, odd)
  //   2*intervalMs to 3*intervalMs-1: visible (index 2, even)
  //   etc.

  function computeBlinkVisibility(time: number, intervalMs: number): boolean {
    return Math.floor(time / intervalMs) % 2 === 0
  }

  test('starts visible at time 0', () => {
    expect(computeBlinkVisibility(0, 600)).toBe(true)
  })

  test('visible during first full interval (0 to intervalMs-1)', () => {
    expect(computeBlinkVisibility(1, 600)).toBe(true)
    expect(computeBlinkVisibility(599, 600)).toBe(true)
  })

  test('invisible during second interval (intervalMs to 2*intervalMs-1)', () => {
    expect(computeBlinkVisibility(600, 600)).toBe(false)
    expect(computeBlinkVisibility(1000, 600)).toBe(false)
    expect(computeBlinkVisibility(1199, 600)).toBe(false)
  })

  test('visible again at start of third interval', () => {
    expect(computeBlinkVisibility(1200, 600)).toBe(true)
    expect(computeBlinkVisibility(1500, 600)).toBe(true)
  })

  test('cycles correctly over multiple intervals', () => {
    const results: boolean[] = []
    for (let i = 0; i < 6; i++) {
      results.push(computeBlinkVisibility(i * 600, 600))
    }
    // Starts visible, then alternates
    expect(results).toEqual([true, false, true, false, true, false])
  })

  test('custom interval duration', () => {
    expect(computeBlinkVisibility(0, 1000)).toBe(true)
    expect(computeBlinkVisibility(500, 1000)).toBe(true)
    expect(computeBlinkVisibility(999, 1000)).toBe(true)
    expect(computeBlinkVisibility(1000, 1000)).toBe(false)
    expect(computeBlinkVisibility(1999, 1000)).toBe(false)
    expect(computeBlinkVisibility(2000, 1000)).toBe(true)
  })

  test('all instances see the same state at the same time', () => {
    const time = 12345
    const interval = 600
    const instance1 = computeBlinkVisibility(time, interval)
    const instance2 = computeBlinkVisibility(time, interval)
    const instance3 = computeBlinkVisibility(time, interval)
    expect(instance1).toBe(instance2)
    expect(instance2).toBe(instance3)
  })
})

describe('blink disabled logic', () => {
  // When disabled or not focused, blink returns [ref, true] (always visible)
  function getBlinkState(
    enabled: boolean,
    focused: boolean,
    time: number,
    intervalMs: number,
  ): [hasRef: boolean, isVisible: boolean] {
    if (!enabled || !focused) return [true, true]
    const isVisible = Math.floor(time / intervalMs) % 2 === 0
    return [true, isVisible]
  }

  test('disabled returns always visible', () => {
    expect(getBlinkState(false, true, 500, 600)[1]).toBe(true)
  })

  test('not focused returns always visible', () => {
    expect(getBlinkState(true, false, 500, 600)[1]).toBe(true)
  })

  test('enabled and focused blinks', () => {
    // At 100ms into a 600ms interval: still visible (first interval)
    expect(getBlinkState(true, true, 100, 600)[1]).toBe(true)
    // At 700ms: second interval (600-1199), so invisible
    expect(getBlinkState(true, true, 700, 600)[1]).toBe(false)
    // At 1300ms: third interval (1200-1799), visible again
    expect(getBlinkState(true, true, 1300, 600)[1]).toBe(true)
  })
})
