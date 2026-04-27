import type { NormalizedUsage } from './schema.js'

function toNumber(v: unknown): number {
  if (typeof v === 'number' && Number.isFinite(v)) return v
  if (typeof v === 'string') {
    const n = Number(v)
    return Number.isFinite(n) ? n : 0
  }
  return 0
}

export function extractUsage(payload: any): NormalizedUsage {
  const usage = payload?.usage ?? {}

  const inputTokens = toNumber(
    usage.input_tokens ?? usage.inputTokens,
  )
  const outputTokens = toNumber(
    usage.output_tokens ?? usage.outputTokens,
  )
  const cacheReadInputTokens = toNumber(
    usage.cache_read_input_tokens ?? usage.cacheReadInputTokens,
  )
  const cacheCreationInputTokens = toNumber(
    usage.cache_creation_input_tokens ?? usage.cacheCreationInputTokens,
  )

  const totalTokens =
    toNumber(usage.total_tokens ?? usage.totalTokens) ||
    inputTokens + outputTokens + cacheReadInputTokens + cacheCreationInputTokens

  return {
    inputTokens,
    outputTokens,
    cacheReadInputTokens,
    cacheCreationInputTokens,
    totalTokens,
  }
}

export function sumUsage(usages: NormalizedUsage[]): NormalizedUsage {
  return usages.reduce(
    (acc, cur) => ({
      inputTokens: acc.inputTokens + cur.inputTokens,
      outputTokens: acc.outputTokens + cur.outputTokens,
      cacheReadInputTokens: acc.cacheReadInputTokens + cur.cacheReadInputTokens,
      cacheCreationInputTokens: acc.cacheCreationInputTokens + cur.cacheCreationInputTokens,
      totalTokens: acc.totalTokens + cur.totalTokens,
    }),
    {
      inputTokens: 0,
      outputTokens: 0,
      cacheReadInputTokens: 0,
      cacheCreationInputTokens: 0,
      totalTokens: 0,
    },
  )
}

export function p50(values: number[]): number | null {
  if (values.length === 0) return null
  const sorted = [...values].sort((a, b) => a - b)
  const mid = Math.floor(sorted.length / 2)
  if (sorted.length % 2 === 1) return sorted[mid]
  return (sorted[mid - 1] + sorted[mid]) / 2
}
