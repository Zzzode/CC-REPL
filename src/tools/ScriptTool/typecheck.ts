import type { CompilerOptions, Diagnostic } from 'typescript'

const VIRTUAL_FILE = '/__script_tool__/inline.ts'

let _typescript: typeof import('typescript') | null = null

async function getTypeScript(): Promise<typeof import('typescript')> {
  if (!_typescript) {
    _typescript = await import('typescript')
  }
  return _typescript
}

function isVirtualFilePath(fileName: string): boolean {
  return fileName === VIRTUAL_FILE || fileName.replaceAll('\\', '/') === VIRTUAL_FILE
}

function categoryToSeverity(
  category: number,
  ts: typeof import('typescript'),
): 'error' | 'warning' | 'info' {
  if (category === ts.DiagnosticCategory.Error) return 'error'
  if (category === ts.DiagnosticCategory.Warning) return 'warning'
  return 'info'
}

export interface ScriptTypeDiagnostic {
  severity: 'error' | 'warning' | 'info'
  code: number
  message: string
  line: number
  column: number
}

export interface ScriptTypeCheckResult {
  passed: boolean
  durationMs: number
  errorCount: number
  warningCount: number
  totalDiagnosticCount: number
  diagnostics: ScriptTypeDiagnostic[]
  truncated: boolean
}

export interface ScriptTypeCheckOptions {
  code: string
  preambleLineCount: number
  maxDiagnostics: number
}

function toScriptDiagnostic(
  diagnostic: Diagnostic,
  ts: typeof import('typescript'),
  preambleLineCount: number,
): ScriptTypeDiagnostic {
  let line = 1
  let column = 1

  if (diagnostic.file && typeof diagnostic.start === 'number') {
    const location = diagnostic.file.getLineAndCharacterOfPosition(
      diagnostic.start,
    )
    line = location.line + 1 - preambleLineCount
    column = location.character + 1
  }

  return {
    severity: categoryToSeverity(diagnostic.category, ts),
    code: diagnostic.code,
    message: ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n'),
    line: Math.max(1, line),
    column,
  }
}

export async function runScriptTypeCheck(
  options: ScriptTypeCheckOptions,
): Promise<ScriptTypeCheckResult> {
  const { code, preambleLineCount, maxDiagnostics } = options
  const start = Date.now()

  try {
    const ts = await getTypeScript()
    const compilerOptions: CompilerOptions = {
      noEmit: true,
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.ESNext,
      moduleResolution: ts.ModuleResolutionKind.Bundler,
      allowJs: true,
      checkJs: false,
      strict: false,
      noImplicitAny: false,
      skipLibCheck: true,
      types: ['node', 'bun-types'],
      lib: ['lib.es2022.d.ts'],
    }

    const host = ts.createCompilerHost(compilerOptions, true)
    const originalGetSourceFile = host.getSourceFile.bind(host)
    const originalReadFile = host.readFile.bind(host)
    const originalFileExists = host.fileExists.bind(host)

    host.getSourceFile = (
      fileName,
      languageVersion,
      onError,
      shouldCreateNewSourceFile,
    ) => {
      if (isVirtualFilePath(fileName)) {
        return ts.createSourceFile(
          fileName,
          code,
          languageVersion,
          true,
          ts.ScriptKind.TS,
        )
      }
      return originalGetSourceFile(
        fileName,
        languageVersion,
        onError,
        shouldCreateNewSourceFile,
      )
    }

    host.readFile = fileName => {
      if (isVirtualFilePath(fileName)) return code
      return originalReadFile(fileName)
    }

    host.fileExists = fileName => {
      if (isVirtualFilePath(fileName)) return true
      return originalFileExists(fileName)
    }

    const program = ts.createProgram([VIRTUAL_FILE], compilerOptions, host)

    const diagnostics = ts
      .getPreEmitDiagnostics(program)
      .filter(diagnostic => {
        if (!diagnostic.file) return true
        return isVirtualFilePath(diagnostic.file.fileName)
      })
      .sort((left, right) => {
        const leftRank = diagnosticRank(left, ts)
        const rightRank = diagnosticRank(right, ts)
        if (leftRank !== rightRank) return leftRank - rightRank
        const leftStart = left.start ?? 0
        const rightStart = right.start ?? 0
        return leftStart - rightStart
      })

    const errorCount = diagnostics.filter(
      diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
    ).length
    const warningCount = diagnostics.filter(
      diagnostic => diagnostic.category === ts.DiagnosticCategory.Warning,
    ).length

    const limitedDiagnostics = diagnostics
      .slice(0, Math.max(1, maxDiagnostics))
      .map(diagnostic => toScriptDiagnostic(diagnostic, ts, preambleLineCount))

    return {
      passed: errorCount === 0,
      durationMs: Date.now() - start,
      errorCount,
      warningCount,
      totalDiagnosticCount: diagnostics.length,
      diagnostics: limitedDiagnostics,
      truncated: diagnostics.length > Math.max(1, maxDiagnostics),
    }
  } catch (error) {
    return {
      passed: false,
      durationMs: Date.now() - start,
      errorCount: 1,
      warningCount: 0,
      totalDiagnosticCount: 1,
      diagnostics: [
        {
          severity: 'error',
          code: -1,
          message: error instanceof Error ? error.message : String(error),
          line: 1,
          column: 1,
        },
      ],
      truncated: false,
    }
  }
}

function diagnosticRank(
  diagnostic: Diagnostic,
  ts: typeof import('typescript'),
): number {
  if (diagnostic.category === ts.DiagnosticCategory.Error) return 0
  if (diagnostic.category === ts.DiagnosticCategory.Warning) return 1
  return 2
}
