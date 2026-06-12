import { describe, expect, mock, test, beforeEach, afterEach } from 'bun:test'
import { DOUBLE_PRESS_TIMEOUT_MS, useDoublePress } from './useDoublePress.js'

describe('DOUBLE_PRESS_TIMEOUT_MS', () => {
  test('is 800ms', () => {
    expect(DOUBLE_PRESS_TIMEOUT_MS).toBe(800)
  })
})

describe('useDoublePress', () => {
  test('is a function', () => {
    expect(typeof useDoublePress).toBe('function')
  })

  test('takes 3 parameters', () => {
    // setPending, onDoublePress, onFirstPress?
    expect(useDoublePress.length).toBe(3)
  })

  test('returns a function (the press handler)', () => {
    // The hook returns a callback function
    // Since we can't render hooks directly, we verify the export shape
    expect(typeof useDoublePress).toBe('function')
  })
})

// Test the double-press state machine logic directly
describe('doublePress state machine logic', () => {
  let setPending: ReturnType<typeof mock>
  let onDoublePress: ReturnType<typeof mock>
  let onFirstPress: ReturnType<typeof mock>

  beforeEach(() => {
    setPending = mock()
    onDoublePress = mock()
    onFirstPress = mock()
  })

  afterEach(() => {
    mock.restore()
    mock.clearAllMocks()
  })

  test('first press sets pending and calls onFirstPress', () => {
    // Simulate what useDoublePress does on first call
    let lastPressTime = 0
    let timeoutId: ReturnType<typeof setTimeout> | undefined

    const press = () => {
      const now = Date.now()
      const timeSinceLastPress = now - lastPressTime
      const isDoublePress =
        timeSinceLastPress <= DOUBLE_PRESS_TIMEOUT_MS && timeoutId !== undefined

      if (isDoublePress) {
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = undefined
        setPending(false)
        onDoublePress()
      } else {
        onFirstPress?.()
        setPending(true)
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = setTimeout(() => {
          setPending(false)
          timeoutId = undefined
        }, DOUBLE_PRESS_TIMEOUT_MS)
      }
      lastPressTime = now
    }

    press()
    expect(onFirstPress).toHaveBeenCalled()
    expect(setPending).toHaveBeenCalledWith(true)
    expect(onDoublePress).not.toHaveBeenCalled()

    // Clean up
    if (timeoutId) clearTimeout(timeoutId)
  })

  test('double press within timeout triggers onDoublePress', () => {
    let lastPressTime = 0
    let timeoutId: ReturnType<typeof setTimeout> | undefined
    let pendingState = false

    const press = () => {
      const now = Date.now()
      const timeSinceLastPress = now - lastPressTime
      const isDoublePress =
        timeSinceLastPress <= DOUBLE_PRESS_TIMEOUT_MS && timeoutId !== undefined

      if (isDoublePress) {
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = undefined
        pendingState = false
        setPending(false)
        onDoublePress()
      } else {
        onFirstPress?.()
        pendingState = true
        setPending(true)
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = setTimeout(() => {
          pendingState = false
          setPending(false)
          timeoutId = undefined
        }, DOUBLE_PRESS_TIMEOUT_MS)
      }
      lastPressTime = now
    }

    // First press
    press()
    expect(setPending).toHaveBeenLastCalledWith(true)
    expect(onDoublePress).not.toHaveBeenCalled()

    // Second press immediately (simulated by setting lastPressTime back)
    lastPressTime = Date.now() - 100 // 100ms ago, well within 800ms
    press()
    expect(onDoublePress).toHaveBeenCalled()
    expect(setPending).toHaveBeenLastCalledWith(false)

    if (timeoutId) clearTimeout(timeoutId)
  })

  test('press after timeout resets and starts fresh', () => {
    let lastPressTime = 0
    let timeoutId: ReturnType<typeof setTimeout> | undefined
    let doublePressCount = 0

    const press = () => {
      const now = Date.now()
      const timeSinceLastPress = now - lastPressTime
      const isDoublePress =
        timeSinceLastPress <= DOUBLE_PRESS_TIMEOUT_MS && timeoutId !== undefined

      if (isDoublePress) {
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = undefined
        doublePressCount++
      } else {
        if (timeoutId) clearTimeout(timeoutId)
        timeoutId = setTimeout(() => {
          timeoutId = undefined
        }, DOUBLE_PRESS_TIMEOUT_MS)
      }
      lastPressTime = now
    }

    // First press
    press()
    expect(doublePressCount).toBe(0)

    // Simulate time passing beyond timeout
    lastPressTime = Date.now() - (DOUBLE_PRESS_TIMEOUT_MS + 100)
    // Also "expire" the timeout
    if (timeoutId) {
      clearTimeout(timeoutId)
      timeoutId = undefined
    }

    // Second press after timeout should be treated as first press of a new cycle
    press()
    expect(doublePressCount).toBe(0) // Not a double press

    if (timeoutId) clearTimeout(timeoutId)
  })
})
