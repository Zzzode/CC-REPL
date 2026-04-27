import { mkdir, writeFile } from 'node:fs/promises'
import path from 'node:path'
import type { ComparisonResult, VariantRun, GroupedSummary } from './schema.js'
import { loadCaseSet } from './caseLoader.js'
import { executeVariant } from './executeRef.js'
import { p50, sumUsage } from './metrics.js'

function aggregate(run: VariantRun) {
  const passCount = run.results.filter((r) => r.pass).length
  const totalCount = run.results.length
  const usage = sumUsage(run.results.map((r) => r.usage))
  const durations = run.results
    .map((r) => r.durationMs)
    .filter((v): v is number => typeof v === 'number')
  return {
    passCount,
    totalCount,
    passRate: totalCount === 0 ? 0 : passCount / totalCount,
    tokens: usage,
    p50DurationMs: p50(durations),
  }
}

function groupedSummary(run: VariantRun): GroupedSummary {
  const byCategory: GroupedSummary['byCategory'] = {}
  const byFrequency: GroupedSummary['byFrequency'] = {}

  for (const r of run.results) {
    const category = String(r.metadata?.category ?? 'uncategorized')
    const frequency = String(r.metadata?.useFrequency ?? 'unspecified')

    byCategory[category] = byCategory[category] ?? { total: 0, pass: 0, tokens: 0 }
    byCategory[category].total += 1
    byCategory[category].pass += r.pass ? 1 : 0
    byCategory[category].tokens += r.usage.totalTokens

    byFrequency[frequency] = byFrequency[frequency] ?? { total: 0, pass: 0, tokens: 0 }
    byFrequency[frequency].total += 1
    byFrequency[frequency].pass += r.pass ? 1 : 0
    byFrequency[frequency].tokens += r.usage.totalTokens
  }

  return { byCategory, byFrequency }
}

export async function runBenchmark(params: {
  casesPath: string
  model?: string
  permissionMode: string
  maxCases?: number
  baseline: { label: string; command: string; args: string[] }
  candidate: { label: string; command: string; args: string[] }
  failOnRegression?: boolean
}): Promise<{ comparison: ComparisonResult; artifactDir: string }> {
  const { caseSet, absolutePath, hash } = await loadCaseSet(params.casesPath)

  const baselineRun = await executeVariant({
    label: params.baseline.label,
    command: params.baseline.command,
    args: params.baseline.args,
    model: params.model,
    permissionMode: params.permissionMode,
    maxCases: params.maxCases,
    casesPath: absolutePath,
    caseSetHash: hash,
    cases: caseSet.cases,
  })

  const candidateRun = await executeVariant({
    label: params.candidate.label,
    command: params.candidate.command,
    args: params.candidate.args,
    model: params.model,
    permissionMode: params.permissionMode,
    maxCases: params.maxCases,
    casesPath: absolutePath,
    caseSetHash: hash,
    cases: caseSet.cases,
  })

  const baselineAgg = aggregate(baselineRun)
  const candidateAgg = aggregate(candidateRun)

  const byIdBase = new Map(baselineRun.results.map((r) => [r.caseId, r]))
  const byIdCand = new Map(candidateRun.results.map((r) => [r.caseId, r]))
  const perCase = caseSet.cases
    .map((c) => {
      const baseline = byIdBase.get(c.id)
      const candidate = byIdCand.get(c.id)
      if (!baseline || !candidate) return null
      return {
        id: c.id,
        name: c.name,
        baseline,
        candidate,
        delta: {
          passChanged: baseline.pass !== candidate.pass,
          totalTokens: candidate.usage.totalTokens - baseline.usage.totalTokens,
          durationMs:
            typeof baseline.durationMs === 'number' && typeof candidate.durationMs === 'number'
              ? candidate.durationMs - baseline.durationMs
              : null,
        },
      }
    })
    .filter((v): v is NonNullable<typeof v> => v !== null)

  const deltaTokens = candidateAgg.tokens.totalTokens - baselineAgg.tokens.totalTokens
  const comparison: ComparisonResult = {
    version: 'pare-benchmark-v1',
    timestamp: new Date().toISOString(),
    config: {
      casesPath: absolutePath,
      caseSetHash: hash,
      model: params.model,
      permissionMode: params.permissionMode,
      baseline: params.baseline,
      candidate: params.candidate,
    },
    aggregate: {
      baseline: baselineAgg,
      candidate: candidateAgg,
      delta: {
        passRate: candidateAgg.passRate - baselineAgg.passRate,
        totalTokens: deltaTokens,
        totalTokensPct:
          baselineAgg.tokens.totalTokens === 0
            ? 0
            : deltaTokens / baselineAgg.tokens.totalTokens,
      },
    },
    grouped: {
      baseline: groupedSummary(baselineRun),
      candidate: groupedSummary(candidateRun),
    },
    perCase,
  }

  const stamp = comparison.timestamp.replace(/[:.]/g, '-')
  const artifactDir = path.resolve(
    process.cwd(),
    '.artifacts',
    'benchmarks',
    'pare',
    stamp,
  )
  await mkdir(artifactDir, { recursive: true })
  await writeFile(
    path.join(artifactDir, 'baseline.json'),
    JSON.stringify(baselineRun, null, 2),
    'utf8',
  )
  await writeFile(
    path.join(artifactDir, 'candidate.json'),
    JSON.stringify(candidateRun, null, 2),
    'utf8',
  )
  await writeFile(
    path.join(artifactDir, 'comparison.json'),
    JSON.stringify(comparison, null, 2),
    'utf8',
  )

  if (params.failOnRegression) {
    const tokenRegression = comparison.aggregate.delta.totalTokens > 0
    const accuracyRegression = comparison.aggregate.delta.passRate < 0
    if (tokenRegression || accuracyRegression) {
      throw new Error(
        'Regression detected (delta tokens=' + comparison.aggregate.delta.totalTokens + ', delta passRate=' + comparison.aggregate.delta.passRate + ')',
      )
    }
  }

  return { comparison, artifactDir }
}
