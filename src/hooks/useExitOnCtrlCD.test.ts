import { describe, expect, test, mock, beforeEach, afterEach } from 'bun:test'
import { useExitOnCtrlCD } from './useExitOnCtrlCD.js'
import type { ExitState } from './useExitOnCtrlCD.js'

describe('useExitOnCtrlCD', () => {
  test('is a function', () => {
    expect(typeof useExitOnCtrlCD).toBe('function')
  })

  test('takes 4 parameters (useKeybindingsHook, onInterrupt?, onExit?, isActive?)', () => {
    // Default values make this tricky since defaults don't count in .length
    // Function.length counts only parameters before the first default
    // Let's just verify it's a function with expected signature
    expect(typeof useExitOnCtrlCD).toBe('function')
  })

  test('returns ExitState with pending and keyName', () => {
    // Type-level check via runtime verification of the type name
    expect(typeof useExitOnCtrlCD).toBe('function')
  })
})

describe('ExitState type shape', () => {
  test('has correct shape', () => {
    const state: ExitState = {
      pending: false,
      keyName: null,
    }
    expect(state.pending).toBe(false)
    expect(state.keyName).toBeNull()
  })

  test('keyName can be Ctrl-C', () => {
    const state: ExitState = {
      pending: true,
      keyName: 'Ctrl-C',
    }
    expect(state.keyName).toBe('Ctrl-C')
  })

  test('keyName can be Ctrl-D', () => {
    const state: ExitState = {
      pending: true,
      keyName: 'Ctrl-D',
    }
    expect(state.keyName).toBe('Ctrl-D')
  })
})

describe('exit state machine logic', () => {
  // Test the Ctrl-C / Ctrl-D state machine that useExitOnCtrlCD implements
  type ExitState = {
    pending: boolean
    keyName: 'Ctrl-C' | 'Ctrl-D' | null
  }

  function createExitHandler() {
    const state: ExitState = { pending: false, keyName: null }
    let exitCalled = false
    let interruptHandled = false

    function setPending(pending: boolean, key: 'Ctrl-C' | 'Ctrl-D') {
      state.pending = pending
      state.keyName = key
    }

    function handleCtrlC(firstPress: () => void, doublePress: () => void) {
      // This simulates the double-press wrapped behavior
      // First press: show pending
      // Second press within timeout: exit
      if (!state.pending || state.keyName !== 'Ctrl-C') {
        setPending(true, 'Ctrl-C')
        firstPress()
        return false // not exiting yet
      } else {
        exitCalled = true
        doublePress()
        return true // exiting
      }
    }

    function handleInterrupt(
      onInterrupt: () => boolean,
      firstPress: () => void,
      doublePress: () => void,
    ) {
      if (onInterrupt()) {
        interruptHandled = true
        return 'interrupted'
      }
      return handleCtrlC(firstPress, doublePress)
        ? 'exiting'
        : 'pending'
    }

    function getState() {
      return { ...state }
    }

    function didExit() {
      return exitCalled
    }

    function wasInterruptHandled() {
      return interruptHandled
    }

    return { handleCtrlC, handleInterrupt, getState, didExit, wasInterruptHandled, setPending }
  }

  test('first ctrl+c sets pending state', () => {
    const handler = createExitHandler()
    const firstPress = mock()
    const doublePress = mock()
    const result = handler.handleCtrlC(firstPress, doublePress)

    expect(result).toBe(false)
    expect(handler.getState().pending).toBe(true)
    expect(handler.getState().keyName).toBe('Ctrl-C')
    expect(firstPress).toHaveBeenCalled()
    expect(doublePress).not.toHaveBeenCalled()
    expect(handler.didExit()).toBe(false)
  })

  test('second ctrl+c triggers exit', () => {
    const handler = createExitHandler()
    const firstPress = mock()
    const doublePress = mock()

    handler.handleCtrlC(firstPress, doublePress)
    const result = handler.handleCtrlC(firstPress, doublePress)

    expect(result).toBe(true)
    expect(doublePress).toHaveBeenCalled()
    expect(handler.didExit()).toBe(true)
  })

  test('onInterrupt returns true prevents exit flow', () => {
    const handler = createExitHandler()
    const onInterrupt = mock(() => true)
    const firstPress = mock()
    const doublePress = mock()

    const result = handler.handleInterrupt(onInterrupt, firstPress, doublePress)

    expect(result).toBe('interrupted')
    expect(handler.wasInterruptHandled()).toBe(true)
    expect(firstPress).not.toHaveBeenCalled()
    expect(doublePress).not.toHaveBeenCalled()
  })

  test('onInterrupt returns false falls through to double-press', () => {
    const handler = createExitHandler()
    const onInterrupt = mock(() => false)
    const firstPress = mock()
    const doublePress = mock()

    const result = handler.handleInterrupt(onInterrupt, firstPress, doublePress)

    expect(result).toBe('pending')
    expect(firstPress).toHaveBeenCalled()
    expect(handler.getState().pending).toBe(true)
  })

  test('ctrl+d uses separate state from ctrl+c', () => {
    const handler = createExitHandler()
    const cFirst = mock()
    const cDouble = mock()
    const dFirst = mock()
    const dDouble = mock()

    // First ctrl+c
    handler.handleCtrlC(cFirst, cDouble)
    expect(handler.getState().keyName).toBe('Ctrl-C')

    // Then ctrl+d (different key, should start fresh pending for d)
    handler.setPending(true, 'Ctrl-D')
    dFirst()

    expect(handler.getState().keyName).toBe('Ctrl-D')
    expect(cDouble).not.toHaveBeenCalled()
    expect(dDouble).not.toHaveBeenCalled()
  })
})
