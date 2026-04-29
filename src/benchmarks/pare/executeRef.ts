import { execa } from 'execa'
import type { PareCase, VariantRun, CaseRunResult, NormalizedUsage } from './schema.js'
import { evaluateAssertion } from './evaluator.js'
import { extractUsage } from './metrics.js'

function tryParseJsonOutput(raw: string): any {
  const trimmed = raw.trim()
  if (!trimmed) return null

  try {
    return JSON.parse(trimmed)
  } catch {
    const lines = trimmed.split(/\r?\n/).map((l) => l.trim()).filter(Boolean)
    for (let i = lines.length - 1; i >= 0; i -= 1) {
      const line = lines[i]
      if (!line.startsWith('{') && !line.startsWith('[')) continue
      try {
        return JSON.parse(line)
      } catch {
        continue
      }
    }
    return null
  }
}

function extractAssistantText(payload: any, fallbackRaw: string): string {
  if (!payload) return fallbackRaw
  if (typeof payload.result === 'string') return payload.result
  if (typeof payload.message === 'string') return payload.message
  if (typeof payload.text === 'string') return payload.text
  const content = payload.message?.content
  if (Array.isArray(content)) {
    const textPart = content.find((p) => typeof p?.text === 'string')
    if (textPart?.text) return textPart.text
  }
  return fallbackRaw
}

function median(values: number[]): number | undefined {
  if (values.length === 0) return undefined
  const sorted = [...values].sort((a, b) => a - b)
  const middle = Math.floor(sorted.length / 2)
  if (sorted.length % 2 === 1) return sorted[middle]
  return (sorted[middle - 1] + sorted[middle]) / 2
}

function medianUsage(runs: CaseRunResult[]): NormalizedUsage {
  const pick = (selector: (u: NormalizedUsage) => number) => {
    const values = runs.map((r) => selector(r.usage)).filter((v) => Number.isFinite(v))
    return median(values) ?? 0
  }
  return {
    inputTokens: pick((u) => u.inputTokens),
    outputTokens: pick((u) => u.outputTokens),
    cacheReadInputTokens: pick((u) => u.cacheReadInputTokens),
    cacheCreationInputTokens: pick((u) => u.cacheCreationInputTokens),
    totalTokens: pick((u) => u.totalTokens),
  }
}

async function executeSingleRun(params: {
  command: string
  args: string[]
  model?: string
  permissionMode: string
  c: PareCase
  cwd?: string
}): Promise<CaseRunResult> {
  const runnerArgs = [
    ...params.args,
    '--print',
    '--output-format',
    'json',
    '--permission-mode',
    params.permissionMode,
    ...(params.model ? ['--model', params.model] : []),
    params.c.prompt,
  ]

  const started = Date.now()
  try {
    const { stdout, stderr, exitCode } = await execa(params.command, runnerArgs, {
      reject: false,
      cwd: params.cwd ?? process.cwd(),
      env: {
        ...process.env,
        DISABLE_TELEMETRY: process.env.DISABLE_TELEMETRY ?? '1',
      },
    })
    const durationMs = Date.now() - started
    const raw = stdout || stderr || ''
    const payload = tryParseJsonOutput(stdout)
    const assistantText = extractAssistantText(payload, raw)
    const assertion = evaluateAssertion(assistantText, params.c.assertion)
    const usage = extractUsage(payload)

    const isError = Boolean(payload?.is_error) || exitCode !== 0
    return {
      caseId: params.c.id,
      caseName: params.c.name,
      pass: !isError && assertion.pass,
      error: isError ? 'process exit code ' + exitCode : assertion.error,
      assistantText,
      rawOutput: raw,
      usage,
      durationMs: typeof payload?.duration_ms === 'number' ? payload.duration_ms : durationMs,
      metadata: params.c.metadata,
      status: 'evaluated',
    }
  } catch (error) {
    const durationMs = Date.now() - started
    return {
      caseId: params.c.id,
      caseName: params.c.name,
      pass: false,
      error: error instanceof Error ? error.message : String(error),
      assistantText: '',
      rawOutput: '',
      usage: {
        inputTokens: 0,
        outputTokens: 0,
        cacheReadInputTokens: 0,
        cacheCreationInputTokens: 0,
        totalTokens: 0,
      },
      durationMs,
      metadata: params.c.metadata,
      status: 'unavailable',
    }
  }
}

export async function executeVariant(params: {
  label: string
  command: string
  args: string[]
  model?: string
  permissionMode: string
  maxCases?: number
  runsPerCase?: number
  casesPath: string
  caseSetHash: string
  cases: PareCase[]
  onCaseResult?: (row: CaseRunResult) => Promise<void> | void
  cwd?: string
}): Promise<VariantRun> {
  const startedAt = new Date().toISOString()
  const selectedCases =
    typeof params.maxCases === 'number'
      ? params.cases.slice(0, params.maxCases)
      : params.cases

  const results: CaseRunResult[] = []
  const totalCases = selectedCases.length
  const runsPerCase = Math.max(1, Math.floor(params.runsPerCase ?? 1))

  for (const [index, c] of selectedCases.entries()) {
    console.log('[pare-benchmark] ' + params.label + ' ' + (index + 1) + '/' + totalCases + ' ' + c.id)

    const runResults: CaseRunResult[] = []
    for (let runIndex = 0; runIndex < runsPerCase; runIndex += 1) {
      const single = await executeSingleRun({
        command: params.command,
        args: params.args,
        model: params.model,
        permissionMode: params.permissionMode,
        c,
        cwd: params.cwd,
      })
      runResults.push(single)
    }

    const passCount = runResults.filter((r) => r.pass).length
    const chosen = runResults[Math.floor((runResults.length - 1) / 2)]
    const row: CaseRunResult = {
      ...chosen,
      pass: passCount >= Math.ceil(runResults.length / 2),
      usage: medianUsage(runResults),
      durationMs: median(runResults.map((r) => r.durationMs ?? 0)) ?? chosen.durationMs,
      status: runResults.every((r) => r.status === 'unavailable' || r.usage.totalTokens === 0) ? 'unavailable' : 'evaluated',
      runs: runResults.map((r) => ({
        pass: r.pass,
        error: r.error,
        usage: r.usage,
        durationMs: r.durationMs,
      })),
    }

    results.push(row)
    if (params.onCaseResult) {
      await params.onCaseResult(row)
    }
  }

  return {
    label: params.label,
    command: params.command,
    args: params.args,
    model: params.model,
    permissionMode: params.permissionMode,
    casesPath: params.casesPath,
    caseSetHash: params.caseSetHash,
    startedAt,
    endedAt: new Date().toISOString(),
    results,
  }
}
