import { execa } from 'execa'
import type { PareCase, VariantRun, CaseRunResult } from './schema.js'
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

export async function executeVariant(params: {
  label: string
  command: string
  args: string[]
  model?: string
  permissionMode: string
  maxCases?: number
  casesPath: string
  caseSetHash: string
  cases: PareCase[]
}): Promise<VariantRun> {
  const startedAt = new Date().toISOString()
  const selectedCases =
    typeof params.maxCases === 'number'
      ? params.cases.slice(0, params.maxCases)
      : params.cases

  const results: CaseRunResult[] = []

  for (const c of selectedCases) {
    const runnerArgs = [
      ...params.args,
      '--print',
      '--output-format',
      'json',
      '--permission-mode',
      params.permissionMode,
      ...(params.model ? ['--model', params.model] : []),
      c.prompt,
    ]

    const started = Date.now()
    try {
      const { stdout, stderr, exitCode } = await execa(params.command, runnerArgs, {
        reject: false,
        env: {
          ...process.env,
          DISABLE_TELEMETRY: process.env.DISABLE_TELEMETRY ?? '1',
        },
      })
      const durationMs = Date.now() - started
      const raw = stdout || stderr || ''
      const payload = tryParseJsonOutput(stdout)
      const assistantText = extractAssistantText(payload, raw)
      const assertion = evaluateAssertion(assistantText, c.assertion)
      const usage = extractUsage(payload)

      const isError = Boolean(payload?.is_error) || exitCode !== 0
      results.push({
        caseId: c.id,
        caseName: c.name,
        pass: !isError && assertion.pass,
        error: isError ? 'process exit code ' + exitCode : assertion.error,
        assistantText,
        rawOutput: raw,
        usage,
        durationMs: typeof payload?.duration_ms === 'number' ? payload.duration_ms : durationMs,
        metadata: c.metadata,
      })
    } catch (error) {
      const durationMs = Date.now() - started
      results.push({
        caseId: c.id,
        caseName: c.name,
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
        metadata: c.metadata,
      })
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
