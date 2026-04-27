import { describe, expect, test } from 'bun:test'
import { resolveRequestedAgentType } from './agentTypeResolution.js'

describe('resolveRequestedAgentType', () => {
  test('returns exact match when present', () => {
    const agents = [{ agentType: 'general-purpose' }, { agentType: 'Plan' }]
    expect(resolveRequestedAgentType('Plan', agents)).toBe('Plan')
  })

  test('matches case-insensitive and separator variants', () => {
    const agents = [{ agentType: 'general-purpose' }]
    expect(resolveRequestedAgentType('General Purpose', agents)).toBe(
      'general-purpose',
    )
  })

  test('maps legacy Explore to namespaced code-explorer when available', () => {
    const agents = [
      { agentType: 'general-purpose' },
      { agentType: 'feature-dev:code-explorer' },
    ]
    expect(resolveRequestedAgentType('Explore', agents)).toBe(
      'feature-dev:code-explorer',
    )
  })

  test('returns undefined for ambiguous legacy alias matches', () => {
    const agents = [
      { agentType: 'team-a:code-explorer' },
      { agentType: 'team-b:code-explorer' },
    ]
    expect(resolveRequestedAgentType('Explore', agents)).toBeUndefined()
  })

  test('returns undefined when there is no compatible match', () => {
    const agents = [{ agentType: 'general-purpose' }]
    expect(resolveRequestedAgentType('non-existent-agent', agents)).toBeUndefined()
  })
})
