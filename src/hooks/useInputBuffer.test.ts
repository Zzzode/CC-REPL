import { describe, expect, test, mock, beforeEach, afterEach } from 'bun:test'
import type { BufferEntry, UseInputBufferProps } from './useInputBuffer.js'

// Test the pure logic from useInputBuffer
// We simulate the state machine behavior

function createInputBuffer(props: UseInputBufferProps) {
  let buffer: BufferEntry[] = []
  let currentIndex = -1
  let lastPushTime = 0
  let pendingPush: ReturnType<typeof setTimeout> | null = null
  let pendingArgs: Parameters<typeof pushToBuffer> | null = null

  function pushToBuffer(
    text: string,
    cursorOffset: number,
    pastedContents: Record<number, any> = {},
  ) {
    const now = Date.now()

    if (pendingPush) {
      clearTimeout(pendingPush)
      pendingPush = null
    }

    if (now - lastPushTime < props.debounceMs) {
      pendingPush = setTimeout(() => {
        // When debounce fires, push with saved args
        const [t, c, p] = pendingArgs!
        _doPush(t, c, p)
        pendingPush = null
        pendingArgs = null
      }, props.debounceMs)
      pendingArgs = [text, cursorOffset, pastedContents]
      return
    }

    _doPush(text, cursorOffset, pastedContents)
  }

  function _doPush(text: string, cursorOffset: number, pastedContents: Record<number, any>) {
    lastPushTime = Date.now()
    const newBuffer = currentIndex >= 0 ? buffer.slice(0, currentIndex + 1) : buffer

    const lastEntry = newBuffer[newBuffer.length - 1]
    if (lastEntry && lastEntry.text === text) {
      return
    }

    const updatedBuffer = [
      ...newBuffer,
      { text, cursorOffset, pastedContents, timestamp: Date.now() },
    ]

    if (updatedBuffer.length > props.maxBufferSize) {
      buffer = updatedBuffer.slice(-props.maxBufferSize)
    } else {
      buffer = updatedBuffer
    }

    currentIndex = currentIndex >= 0 ? currentIndex + 1 : buffer.length - 1
    if (currentIndex >= props.maxBufferSize) {
      currentIndex = props.maxBufferSize - 1
    }
  }

  function undo(): BufferEntry | undefined {
    if (currentIndex < 0 || buffer.length === 0) {
      return undefined
    }

    const targetIndex = Math.max(0, currentIndex - 1)
    const entry = buffer[targetIndex]

    if (entry) {
      currentIndex = targetIndex
      return entry
    }

    return undefined
  }

  function clearBuffer() {
    buffer = []
    currentIndex = -1
    lastPushTime = 0
    if (pendingPush) {
      clearTimeout(pendingPush)
      pendingPush = null
    }
  }

  function canUndo() {
    return currentIndex > 0 && buffer.length > 1
  }

  function getBuffer() {
    return [...buffer]
  }

  function getCurrentIndex() {
    return currentIndex
  }

  function flushPending() {
    if (pendingPush && pendingArgs) {
      clearTimeout(pendingPush)
      _doPush(pendingArgs[0], pendingArgs[1], pendingArgs[2])
      pendingPush = null
      pendingArgs = null
    }
  }

  return {
    pushToBuffer,
    undo,
    canUndo,
    clearBuffer,
    getBuffer,
    getCurrentIndex,
    flushPending,
  }
}

describe('useInputBuffer logic', () => {
  test('push adds entries to buffer', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('hello', 5)
    buf.pushToBuffer('world', 5)

    const entries = buf.getBuffer()
    expect(entries.length).toBe(2)
    expect(entries[0].text).toBe('hello')
    expect(entries[1].text).toBe('world')
  })

  test('duplicate consecutive entries are not added', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('hello', 5)
    buf.pushToBuffer('hello', 5)

    expect(buf.getBuffer().length).toBe(1)
  })

  test('maxBufferSize limits buffer size', () => {
    const buf = createInputBuffer({ maxBufferSize: 3, debounceMs: 0 })
    for (let i = 0; i < 10; i++) {
      buf.pushToBuffer(`entry-${i}`, i)
    }

    const entries = buf.getBuffer()
    expect(entries.length).toBe(3)
    expect(entries[0].text).toBe('entry-7')
    expect(entries[2].text).toBe('entry-9')
  })

  test('undo returns previous entry', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('first', 5)
    buf.pushToBuffer('second', 6)
    buf.pushToBuffer('third', 5)

    expect(buf.canUndo()).toBe(true)

    const result = buf.undo()
    expect(result?.text).toBe('second')
    expect(buf.getCurrentIndex()).toBe(1)
  })

  test('undo from first entry returns first entry (canUndo false)', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('only', 4)

    // canUndo is false when at first entry
    expect(buf.canUndo()).toBe(false)
    // undo still returns the first entry (at index 0) - it's the same as current
    const result = buf.undo()
    expect(result?.text).toBe('only')
    expect(buf.getCurrentIndex()).toBe(0)
  })

  test('empty buffer cannot undo', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    expect(buf.canUndo()).toBe(false)
    expect(buf.undo()).toBeUndefined()
  })

  test('clearBuffer resets everything', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('a', 1)
    buf.pushToBuffer('b', 1)

    buf.clearBuffer()

    expect(buf.getBuffer().length).toBe(0)
    expect(buf.getCurrentIndex()).toBe(-1)
    expect(buf.canUndo()).toBe(false)
  })

  test('push after undo truncates forward history', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('a', 1)
    buf.pushToBuffer('b', 1)
    buf.pushToBuffer('c', 1)

    buf.undo() // at index 1 ('b')
    buf.undo() // at index 0 ('a')

    buf.pushToBuffer('d', 1)

    const entries = buf.getBuffer()
    expect(entries.length).toBe(2)
    expect(entries[0].text).toBe('a')
    expect(entries[1].text).toBe('d')
  })

  test('debounce delays rapid pushes', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 1000 })
    buf.pushToBuffer('first', 5)
    buf.pushToBuffer('second', 6) // Should be debounced

    // Only first entry should be in buffer immediately
    expect(buf.getBuffer().length).toBe(1)
    expect(buf.getBuffer()[0].text).toBe('first')

    // Flush pending to simulate debounce timer firing
    buf.flushPending()
    expect(buf.getBuffer().length).toBe(2)
    expect(buf.getBuffer()[1].text).toBe('second')
  })

  test('debounced push cancels previous pending push', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 1000 })
    buf.pushToBuffer('first', 5)
    buf.pushToBuffer('second', 6)
    buf.pushToBuffer('third', 5)

    // Only first entry immediately
    expect(buf.getBuffer().length).toBe(1)

    // After flush, only the latest debounced entry should be added
    buf.flushPending()
    expect(buf.getBuffer().length).toBe(2)
    expect(buf.getBuffer()[1].text).toBe('third')
  })

  test('cursorOffset is preserved in entries', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    buf.pushToBuffer('hello', 3)

    expect(buf.getBuffer()[0].cursorOffset).toBe(3)
  })

  test('pastedContents are preserved in entries', () => {
    const buf = createInputBuffer({ maxBufferSize: 10, debounceMs: 0 })
    const pasted = { 0: { type: 'image', data: 'abc' } }
    buf.pushToBuffer('hello', 5, pasted)

    expect(buf.getBuffer()[0].pastedContents).toEqual(pasted)
  })
})
