import { describe, expect, test, beforeEach, afterEach, mock } from 'bun:test'

// Test useMinDisplayTime logic
// The hook throttles values so each stays visible for at least minMs

describe('useMinDisplayTime logic', () => {
  function createMinDisplayTime<T>(initialValue: T, minMs: number) {
    let displayed = initialValue
    let lastShownAt = Date.now() // Track when current value was first shown
    let timer: ReturnType<typeof setTimeout> | null = null
    let pendingValue: T | null = null

    function setValue(value: T) {
      const elapsed = Date.now() - lastShownAt
      if (elapsed >= minMs) {
        lastShownAt = Date.now()
        displayed = value
        return
      }
      if (timer) clearTimeout(timer)
      pendingValue = value
      timer = setTimeout(() => {
        lastShownAt = Date.now()
        displayed = pendingValue as T
        timer = null
        pendingValue = null
      }, minMs - elapsed)
    }

    function getDisplayed() {
      return displayed
    }

    function flushPending() {
      if (timer && pendingValue !== null) {
        clearTimeout(timer)
        lastShownAt = Date.now()
        displayed = pendingValue
        timer = null
        pendingValue = null
      }
    }

    function cleanup() {
      if (timer) clearTimeout(timer)
    }

    return { setValue, getDisplayed, flushPending, cleanup }
  }

  test('initial value is displayed immediately', () => {
    const { getDisplayed, cleanup } = createMinDisplayTime('initial', 1000)
    expect(getDisplayed()).toBe('initial')
    cleanup()
  })

  test('value changes immediately when minMs has elapsed', () => {
    const { setValue, getDisplayed, cleanup } = createMinDisplayTime('a', 10)
    // First value sets lastShownAt
    // Wait for minMs to pass
    const start = Date.now()
    while (Date.now() - start < 15) {
      // busy wait - short time for test
    }
    setValue('b')
    expect(getDisplayed()).toBe('b')
    cleanup()
  })

  test('fast changes are delayed', () => {
    const { setValue, getDisplayed, cleanup } = createMinDisplayTime('a', 1000)
    setValue('b')
    // Should still show 'a' because minMs hasn't elapsed
    expect(getDisplayed()).toBe('a')
    cleanup()
  })
})

describe('useDeferredHookMessages logic', () => {
  // Test the deferred message state machine
  function createDeferredHookMessages<T extends object>(
    pendingPromise: Promise<T[]> | undefined,
    onSetMessages: (msgs: T[]) => void,
  ) {
    let pending = pendingPromise ?? null
    let resolved = !pendingPromise

    if (pending) {
      pending.then(msgs => {
        resolved = true
        pending = null
        if (msgs.length > 0) {
          onSetMessages(msgs)
        }
      })
    }

    async function ensureResolved(): Promise<void> {
      if (resolved || !pending) return
      const msgs = await pending
      if (resolved) return
      resolved = true
      pending = null
      if (msgs.length > 0) {
        onSetMessages(msgs)
      }
    }

    function isResolved() {
      return resolved
    }

    return { ensureResolved, isResolved }
  }

  test('starts in resolved state when no promise provided', () => {
    const { isResolved } = createDeferredHookMessages(undefined, () => {})
    expect(isResolved()).toBe(true)
  })

  test('starts in pending state when promise provided', async () => {
    let resolve: (msgs: string[]) => void = () => {}
    const promise = new Promise<string[]>(r => {
      resolve = r
    })
    const { isResolved, ensureResolved } = createDeferredHookMessages(promise, () => {})

    expect(isResolved()).toBe(false)

    resolve(['msg1'])
    await promise

    // Wait a tick for the then handler to run
    await new Promise(r => setTimeout(r, 0))

    expect(isResolved()).toBe(true)
  })

  test('calls onSetMessages when promise resolves with messages', async () => {
    let resolve: (msgs: string[]) => void = () => {}
    const promise = new Promise<string[]>(r => {
      resolve = r
    })

    let receivedMessages: string[] | null = null
    const { ensureResolved } = createDeferredHookMessages(promise, msgs => {
      receivedMessages = msgs
    })

    resolve(['hello', 'world'])
    await promise
    await new Promise(r => setTimeout(r, 0))

    expect(receivedMessages).toEqual(['hello', 'world'])
  })

  test('does not call onSetMessages for empty messages', async () => {
    let resolve: (msgs: string[]) => void = () => {}
    const promise = new Promise<string[]>(r => {
      resolve = r
    })

    let called = false
    createDeferredHookMessages(promise, () => {
      called = true
    })

    resolve([])
    await promise
    await new Promise(r => setTimeout(r, 0))

    expect(called).toBe(false)
  })

  test('ensureResolved waits for and returns messages', async () => {
    let resolve: (msgs: string[]) => void = () => {}
    const promise = new Promise<string[]>(r => {
      resolve = r
    })

    let receivedMessages: string[] | null = null
    const { ensureResolved, isResolved } = createDeferredHookMessages(promise, msgs => {
      receivedMessages = msgs
    })

    setTimeout(() => resolve(['async-msg']), 10)
    await ensureResolved()

    expect(isResolved()).toBe(true)
    expect(receivedMessages).toEqual(['async-msg'])
  })
})
