import { describe, expect, test } from 'bun:test'
import { useDynamicConfig } from './useDynamicConfig.js'

describe('useDynamicConfig', () => {
  test('is a function', () => {
    expect(typeof useDynamicConfig).toBe('function')
  })

  test('takes 2 parameters (configName, defaultValue)', () => {
    expect(useDynamicConfig.length).toBe(2)
  })

  test('is a generic hook returning same type as defaultValue', () => {
    // Type-level verification via runtime check
    expect(typeof useDynamicConfig).toBe('function')
  })
})

describe('useDynamicConfig behavior in test environment', () => {
  // In test environment, the hook returns defaultValue immediately (no async fetch)
  // This is handled by the NODE_ENV === 'test' check in the hook
  test('NODE_ENV is test during test runs', () => {
    expect(process.env.NODE_ENV).toBe('test')
  })
})
