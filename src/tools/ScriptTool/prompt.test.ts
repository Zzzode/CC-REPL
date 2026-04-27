import { describe, expect, test } from 'bun:test'
import { getPrompt } from './prompt.js'

describe('ScriptTool prompt agent guidance', () => {
  test('renders session agent list when provided', () => {
    const prompt = getPrompt({
      agents: [
        {
          agentType: 'general-purpose',
          whenToUse: 'Handle general tasks.',
        },
        {
          agentType: 'feature-dev:code-explorer',
          whenToUse: 'Search and inspect code.',
        },
      ],
    })

    expect(prompt).toContain('Available agent types in this session:')
    expect(prompt).toContain('`general-purpose`')
    expect(prompt).toContain('`feature-dev:code-explorer`')
    expect(prompt).toContain("subagent_type: 'feature-dev:code-explorer'")
  })

  test('falls back to general-purpose when no agent list is available', () => {
    const prompt = getPrompt()

    expect(prompt).toContain('Available agent types depend on the current session configuration.')
    expect(prompt).toContain("subagent_type: 'general-purpose'")
  })
})
