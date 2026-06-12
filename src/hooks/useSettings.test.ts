import { describe, expect, test } from 'bun:test'
import { useSettings } from './useSettings.js'
import type { ReadonlySettings } from './useSettings.js'

describe('useSettings', () => {
  test('is a function', () => {
    expect(typeof useSettings).toBe('function')
  })

  test('takes no parameters', () => {
    expect(useSettings.length).toBe(0)
  })
})

describe('ReadonlySettings type', () => {
  test('type export exists', () => {
    // We can't test types at runtime, but we can verify the module exports
    // are consistent with what we expect.
    // The ReadonlySettings type is a type-only export so it won't exist at runtime.
    expect(typeof useSettings).toBe('function')
  })
})
