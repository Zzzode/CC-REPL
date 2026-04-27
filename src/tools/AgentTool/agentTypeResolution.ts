type AgentLike = {
  agentType: string
}

// Legacy aliases used by older prompts/examples.
// Values are canonical agent type fragments to match against.
const LEGACY_AGENT_TYPE_ALIASES: Readonly<Record<string, readonly string[]>> = {
  explore: ['explore', 'code-explorer'],
  explorer: ['explore', 'code-explorer', 'explorer'],
  plan: ['plan'],
  planner: ['plan'],
}

function canonicalizeAgentType(agentType: string): string {
  return agentType.trim().toLowerCase().replaceAll('_', '-').replaceAll(' ', '-')
}

function findCanonicalMatch(
  requestedType: string,
  agents: readonly AgentLike[],
): AgentLike | undefined {
  const canonicalRequested = canonicalizeAgentType(requestedType)
  return agents.find(
    agent => canonicalizeAgentType(agent.agentType) === canonicalRequested,
  )
}

function getAliasCandidates(requestedType: string): readonly string[] {
  const canonicalRequested = canonicalizeAgentType(requestedType)
  return LEGACY_AGENT_TYPE_ALIASES[canonicalRequested] ?? [canonicalRequested]
}

/**
 * Resolves legacy / loosely-cased agent type inputs into concrete active types.
 * Returns undefined when no unambiguous compatible match is found.
 */
export function resolveRequestedAgentType(
  requestedType: string | undefined,
  agents: readonly AgentLike[],
): string | undefined {
  if (!requestedType) {
    return undefined
  }

  const trimmed = requestedType.trim()
  if (!trimmed) {
    return undefined
  }

  const exact = agents.find(agent => agent.agentType === trimmed)
  if (exact) {
    return exact.agentType
  }

  const canonical = findCanonicalMatch(trimmed, agents)
  if (canonical) {
    return canonical.agentType
  }

  const aliasCandidates = getAliasCandidates(trimmed)

  for (const alias of aliasCandidates) {
    const aliasMatch = findCanonicalMatch(alias, agents)
    if (aliasMatch) {
      return aliasMatch.agentType
    }
  }

  for (const alias of aliasCandidates) {
    const suffix = `:${alias}`
    const suffixMatches = agents.filter(agent =>
      canonicalizeAgentType(agent.agentType).endsWith(suffix),
    )
    if (suffixMatches.length === 1) {
      return suffixMatches[0]!.agentType
    }
  }

  return undefined
}
