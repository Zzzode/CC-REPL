import { z } from 'zod'

const AssertionExactSchema = z.object({
  type: z.literal('exact'),
  expected: z.string(),
  normalize: z.array(z.enum(['trim', 'lower', 'collapseWhitespace'])).optional(),
})

const AssertionRegexSchema = z.object({
  type: z.literal('regex'),
  pattern: z.string(),
  flags: z.string().optional(),
})

const AssertionIncludesSchema = z.object({
  type: z.literal('includes'),
  expected: z.string(),
  normalize: z.array(z.enum(['trim', 'lower', 'collapseWhitespace'])).optional(),
})

export const AssertionSchema = z.discriminatedUnion('type', [
  AssertionExactSchema,
  AssertionRegexSchema,
  AssertionIncludesSchema,
])

const CaseMetadataSchema = z.object({
  pareScenarioId: z.string().optional(),
  pareScenarioName: z.string().optional(),
  category: z.string().optional(),
  useFrequency: z.enum(['very_high', 'high', 'medium', 'low', 'very_low']).optional(),
  command: z.string().optional(),
}).passthrough()

export const CaseSchema = z.object({
  id: z.string().min(1),
  name: z.string().min(1),
  prompt: z.string().min(1),
  assertion: AssertionSchema,
  tags: z.array(z.string()).optional(),
  metadata: CaseMetadataSchema.optional(),
})

const CaseSetSourceSchema = z.object({
  origin: z.string().optional(),
  migrationDate: z.string().optional(),
  note: z.string().optional(),
}).passthrough()

export const CaseSetSchema = z.object({
  version: z.literal('pare-case-v1'),
  source: CaseSetSourceSchema.optional(),
  cases: z.array(CaseSchema).min(1),
})

export type PareAssertion = z.infer<typeof AssertionSchema>
export type PareCase = z.infer<typeof CaseSchema>
export type PareCaseSet = z.infer<typeof CaseSetSchema>

export type NormalizedUsage = {
  inputTokens: number
  outputTokens: number
  cacheReadInputTokens: number
  cacheCreationInputTokens: number
  totalTokens: number
}

export type CaseRunStatus = 'evaluated' | 'skipped' | 'unavailable'

export type CaseRunResult = {
  caseId: string
  caseName: string
  pass: boolean
  error?: string
  assistantText: string
  rawOutput: string
  usage: NormalizedUsage
  durationMs?: number
  metadata?: Record<string, unknown>
  status?: CaseRunStatus
  runs?: Array<{
    pass: boolean
    error?: string
    usage: NormalizedUsage
    durationMs?: number
  }>
}

export type VariantRun = {
  label: string
  command: string
  args: string[]
  model?: string
  permissionMode?: string
  casesPath: string
  caseSetHash: string
  startedAt: string
  endedAt: string
  results: CaseRunResult[]
}

export type GroupedSummary = {
  byCategory: Record<string, { total: number; pass: number; tokens: number }>
  byFrequency: Record<string, { total: number; pass: number; tokens: number }>
}

export type ComparisonResult = {
  version: 'pare-benchmark-v1'
  timestamp: string
  config: {
    casesPath: string
    caseSetHash: string
    model?: string
    permissionMode: string
    baseline: { label: string; command: string; args: string[] }
    candidate: { label: string; command: string; args: string[] }
  }
  aggregate: {
    baseline: {
      passRate: number
      passCount: number
      totalCount: number
      tokens: NormalizedUsage
      p50DurationMs: number | null
      totalDurationMs: number
      avgDurationMs: number
      errorCount: number
      errorRate: number
      topErrors: Array<{ message: string; count: number }>
    }
    candidate: {
      passRate: number
      passCount: number
      totalCount: number
      tokens: NormalizedUsage
      p50DurationMs: number | null
      totalDurationMs: number
      avgDurationMs: number
      errorCount: number
      errorRate: number
      topErrors: Array<{ message: string; count: number }>
    }
    delta: {
      passRate: number
      totalTokens: number
      totalTokensPct: number
      totalDurationMs: number
      totalDurationPct: number
      avgDurationMs: number
      avgDurationPct: number
    }
    v2?: {
      runsPerCase: number
      evaluatedCases: number
      skippedCases: number
      unavailableCases: number
      weightedTokenReductionPct: number
    }
  }
  grouped: {
    baseline: GroupedSummary
    candidate: GroupedSummary
  }
  perCase: Array<{
    id: string
    name: string
    baseline: CaseRunResult
    candidate: CaseRunResult
    delta: {
      passChanged: boolean
      totalTokens: number
      durationMs: number | null
    }
  }>
}
