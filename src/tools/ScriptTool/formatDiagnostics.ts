import type { ScriptTypeCheckResult } from './typecheck.js'

// Transpiler error line numbers include preamble offset; subtract it to recover real user-code line numbers.
export function adjustLineNumbers(message: string, offset: number): string {
  return message.replace(
    /\bline\s+(\d+)/gi,
    (match, n) => {
      const adjusted = Number(n) - offset
      return adjusted >= 1 ? `line ${adjusted}` : match
    },
  )
}

export interface BuildMessageLike {
  message: string
  position?: {
    line: number
    column: number
    lineText: string
    length: number
  } | null
}

export function isBuildMessage(value: unknown): value is BuildMessageLike {
  return (
    value != null &&
    typeof value === 'object' &&
    'message' in value &&
    typeof (value as BuildMessageLike).message === 'string'
  )
}

// Extract BuildMessage list from exceptions thrown by Bun.Transpiler.
// Single error -> BuildMessage; multiple errors -> AggregateError { errors: BuildMessage[] }.
export function collectBuildMessages(error: unknown): BuildMessageLike[] {
  if (
    error != null &&
    typeof error === 'object' &&
    'errors' in error &&
    Array.isArray((error as AggregateError).errors)
  ) {
    return (error as AggregateError).errors.filter(isBuildMessage)
  }
  if (isBuildMessage(error)) {
    return [error]
  }
  return []
}

// Use structured BuildMessage position info to format diagnostics with line/column and source lines.
// When BuildMessage is unavailable, fall back to plain-text adjustLineNumbers handling.
export function formatSyntaxError(error: unknown, preambleLineCount: number): string {
  const messages = collectBuildMessages(error)

  if (messages.length === 0) {
    const raw = error instanceof Error ? error.message : String(error)
    return `Syntax error: ${adjustLineNumbers(raw, preambleLineCount)}`
  }

  const header =
    messages.length === 1
      ? 'Syntax error (1 diagnostic):'
      : `Syntax errors (${messages.length} diagnostics):`

  const lines = [header]

  for (const msg of messages) {
    const pos = msg.position
    if (pos) {
      const adjustedLine = Math.max(1, pos.line - preambleLineCount)
      lines.push(`  line ${adjustedLine}, col ${pos.column}: ${msg.message}`)

      if (pos.lineText) {
        lines.push(`    ${pos.lineText}`)
        const caretPad = Math.max(0, pos.column - 1)
        const caretLen = Math.max(1, pos.length)
        lines.push(`    ${' '.repeat(caretPad)}${'^'.repeat(caretLen)}`)
      }
    } else {
      lines.push(`  ${msg.message}`)
    }
  }

  return lines.join('\n')
}

export function formatTypeCheckFailure(result: ScriptTypeCheckResult): string {
  const lines = [
    `Type check failed with ${result.errorCount} error(s) in ${result.durationMs}ms.`,
  ]

  const errorDiagnostics = result.diagnostics.filter(
    diagnostic => diagnostic.severity === 'error',
  )

  for (const diagnostic of errorDiagnostics) {
    lines.push(
      `line ${diagnostic.line}, col ${diagnostic.column} TS${diagnostic.code}: ${diagnostic.message}`,
    )
  }

  if (result.truncated) {
    const remaining = Math.max(
      0,
      result.totalDiagnosticCount - result.diagnostics.length,
    )
    if (remaining > 0) {
      lines.push(`... and ${remaining} more diagnostic(s).`)
    }
  }

  return lines.join('\n')
}
