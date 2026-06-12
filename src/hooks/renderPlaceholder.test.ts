import { describe, expect, test } from 'bun:test'
import { renderPlaceholder } from './renderPlaceholder.js'
import chalk from 'chalk'

describe('renderPlaceholder', () => {
  describe('showPlaceholder', () => {
    test('is true when value is empty and placeholder exists', () => {
      const result = renderPlaceholder({
        placeholder: 'Type something...',
        value: '',
        terminalFocus: true,
      })
      expect(result.showPlaceholder).toBe(true)
    })

    test('is false when value is non-empty', () => {
      const result = renderPlaceholder({
        placeholder: 'Type something...',
        value: 'hello',
        terminalFocus: true,
      })
      expect(result.showPlaceholder).toBe(false)
    })

    test('is false when placeholder is undefined', () => {
      const result = renderPlaceholder({
        placeholder: undefined,
        value: '',
        terminalFocus: true,
      })
      expect(result.showPlaceholder).toBe(false)
    })

    test('is false when placeholder is empty string', () => {
      const result = renderPlaceholder({
        placeholder: '',
        value: '',
        terminalFocus: true,
      })
      expect(result.showPlaceholder).toBe(false)
    })
  })

  describe('renderedPlaceholder', () => {
    test('is undefined when no placeholder', () => {
      const result = renderPlaceholder({
        placeholder: undefined,
        value: '',
        terminalFocus: true,
      })
      expect(result.renderedPlaceholder).toBeUndefined()
    })

    test('returns dimmed placeholder text when no cursor', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: false,
        focus: false,
        terminalFocus: true,
      })
      expect(result.renderedPlaceholder).toBe(chalk.dim('hello'))
    })

    test('returns dimmed placeholder when focus is false', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: true,
        focus: false,
        terminalFocus: true,
      })
      expect(result.renderedPlaceholder).toBe(chalk.dim('hello'))
    })

    test('returns dimmed placeholder when terminalFocus is false', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: false,
      })
      expect(result.renderedPlaceholder).toBe(chalk.dim('hello'))
    })

    test('inverts first character when cursor is shown and focused', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: true,
      })
      const expected = chalk.inverse('h') + chalk.dim('ello')
      expect(result.renderedPlaceholder).toBe(expected)
    })

    test('empty string placeholder returns undefined for renderedPlaceholder', () => {
      const result = renderPlaceholder({
        placeholder: '',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: true,
      })
      // Empty string is falsy, so the placeholder block is skipped entirely
      expect(result.renderedPlaceholder).toBeUndefined()
      expect(result.showPlaceholder).toBe(false)
    })

    test('hidePlaceholderText shows only cursor space', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: true,
        hidePlaceholderText: true,
      })
      expect(result.renderedPlaceholder).toBe(chalk.inverse(' '))
    })

    test('hidePlaceholderText with no cursor shows empty string', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: '',
        showCursor: false,
        focus: true,
        terminalFocus: true,
        hidePlaceholderText: true,
      })
      expect(result.renderedPlaceholder).toBe('')
    })

    test('uses custom invert function', () => {
      const customInvert = (text: string) => `[${text}]`
      const result = renderPlaceholder({
        placeholder: 'hi',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: true,
        invert: customInvert,
      })
      const expected = '[h]' + chalk.dim('i')
      expect(result.renderedPlaceholder).toBe(expected)
    })
  })

  describe('edge cases', () => {
    test('single character placeholder with cursor', () => {
      const result = renderPlaceholder({
        placeholder: 'x',
        value: '',
        showCursor: true,
        focus: true,
        terminalFocus: true,
      })
      expect(result.renderedPlaceholder).toBe(chalk.inverse('x') + chalk.dim(''))
    })

    test('value does not affect renderedPlaceholder (only showPlaceholder)', () => {
      const result = renderPlaceholder({
        placeholder: 'hello',
        value: 'world',
        showCursor: true,
        focus: true,
        terminalFocus: true,
      })
      // renderedPlaceholder is still computed, just showPlaceholder is false
      expect(result.renderedPlaceholder).toBe(chalk.inverse('h') + chalk.dim('ello'))
      expect(result.showPlaceholder).toBe(false)
    })
  })
})
