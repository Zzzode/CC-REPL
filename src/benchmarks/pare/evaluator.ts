import type { PareAssertion } from './schema.js'

function normalize(value: string, modes: Array<'trim' | 'lower' | 'collapseWhitespace'> = []): string {
  let out = value
  if (modes.includes('trim')) out = out.trim()
  if (modes.includes('collapseWhitespace')) out = out.replace(/\s+/g, ' ')
  if (modes.includes('lower')) out = out.toLowerCase()
  return out
}

export function evaluateAssertion(text: string, assertion: PareAssertion): { pass: boolean; error?: string } {
  if (assertion.type === 'exact') {
    const actual = normalize(text, assertion.normalize)
    const expected = normalize(assertion.expected, assertion.normalize)
    return actual === expected
      ? { pass: true }
      : { pass: false, error: 'expected exact match' }
  }

  if (assertion.type === 'includes') {
    const actual = normalize(text, assertion.normalize)
    const expected = normalize(assertion.expected, assertion.normalize)
    return actual.includes(expected)
      ? { pass: true }
      : { pass: false, error: 'expected text to include substring' }
  }

  const regex = new RegExp(assertion.pattern, assertion.flags)
  return regex.test(text)
    ? { pass: true }
    : { pass: false, error: 'expected text to match regex' }
}
