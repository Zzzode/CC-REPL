import { describe, expect, test } from 'bun:test'
import {
  adjustLineNumbers,
  collectBuildMessages,
  formatSyntaxError,
  formatTypeCheckFailure,
  isBuildMessage,
  type BuildMessageLike,
} from './formatDiagnostics.js'
import type { ScriptTypeCheckResult } from './typecheck.js'

// ---------- Helpers: build real BuildMessage / AggregateError with Bun.Transpiler ----------

const transpiler = new Bun.Transpiler({ loader: 'ts', target: 'bun' })

function catchTranspileError(code: string): unknown {
  try {
    transpiler.transformSync(code)
    return null
  } catch (e) {
    return e
  }
}

// ---------- adjustLineNumbers ----------

describe('adjustLineNumbers', () => {
  test('减去偏移量还原用户代码行号', () => {
    expect(adjustLineNumbers('error at line 10', 6)).toBe('error at line 4')
  })

  test('不修正调整后行号小于 1 的情况', () => {
    expect(adjustLineNumbers('error at line 2', 5)).toBe('error at line 2')
  })

  test('处理多个行号引用', () => {
    const msg = 'error at line 8, related to line 12'
    expect(adjustLineNumbers(msg, 4)).toBe(
      'error at line 4, related to line 8',
    )
  })

  test('无行号引用时原样返回', () => {
    const msg = 'Unexpected token ;'
    expect(adjustLineNumbers(msg, 6)).toBe(msg)
  })

  test('偏移为 0 时不改变行号', () => {
    expect(adjustLineNumbers('line 5', 0)).toBe('line 5')
  })
})

// ---------- isBuildMessage ----------

describe('isBuildMessage', () => {
  test('识别合法的 BuildMessage 结构', () => {
    expect(isBuildMessage({ message: 'foo', position: null })).toBe(true)
  })

  test('只要有 string 类型的 message 属性即可', () => {
    expect(isBuildMessage({ message: 'bar' })).toBe(true)
  })

  test('拒绝 null', () => {
    expect(isBuildMessage(null)).toBe(false)
  })

  test('拒绝非对象', () => {
    expect(isBuildMessage('string')).toBe(false)
    expect(isBuildMessage(42)).toBe(false)
  })

  test('拒绝 message 非字符串的对象', () => {
    expect(isBuildMessage({ message: 123 })).toBe(false)
  })
})

// ---------- collectBuildMessages ----------

describe('collectBuildMessages', () => {
  test('从单个 BuildMessage 收集', () => {
    const msg: BuildMessageLike = { message: 'err', position: null }
    expect(collectBuildMessages(msg)).toEqual([msg])
  })

  test('从 AggregateError 收集所有 BuildMessage', () => {
    const msgs: BuildMessageLike[] = [
      { message: 'a', position: null },
      { message: 'b', position: null },
    ]
    const agg = new AggregateError(msgs, 'Parse error')
    expect(collectBuildMessages(agg)).toEqual(msgs)
  })

  test('过滤掉 AggregateError.errors 中的非 BuildMessage', () => {
    const valid: BuildMessageLike = { message: 'ok', position: null }
    const agg = new AggregateError([valid, 42, null, 'str'], 'mixed')
    expect(collectBuildMessages(agg)).toEqual([valid])
  })

  test('Error 对象也满足 BuildMessageLike（有 string 类型 message）', () => {
    expect(collectBuildMessages(new Error('plain'))).toEqual([
      new Error('plain'),
    ])
  })

  test('非对象类型返回空数组', () => {
    expect(collectBuildMessages('string error')).toEqual([])
    expect(collectBuildMessages(null)).toEqual([])
    expect(collectBuildMessages(42)).toEqual([])
  })
})

// ---------- formatSyntaxError (mock data) ----------

describe('formatSyntaxError with mock data', () => {
  test('单个 BuildMessage 包含 position 输出行列号和 caret', () => {
    const error: BuildMessageLike = {
      message: 'Unexpected ;',
      position: {
        line: 9,
        column: 19,
        lineText: 'const x: number = ;',
        length: 1,
      },
    }
    const result = formatSyntaxError(error, 6)
    expect(result).toContain('Syntax error (1 diagnostic):')
    expect(result).toContain('line 3, col 19: Unexpected ;')
    expect(result).toContain('const x: number = ;')
    expect(result).toContain('                  ^')
  })

  test('行号修正后不小于 1（边界保护）', () => {
    const error: BuildMessageLike = {
      message: 'Bad token',
      position: { line: 2, column: 1, lineText: 'x', length: 1 },
    }
    const result = formatSyntaxError(error, 10)
    expect(result).toContain('line 1,')
  })

  test('position 为 null 时只输出消息文本', () => {
    const error: BuildMessageLike = { message: 'Unknown error', position: null }
    const result = formatSyntaxError(error, 0)
    expect(result).toContain('Unknown error')
  })

  test('多个 BuildMessage 输出诊断计数和每项详情', () => {
    const errors: BuildMessageLike[] = [
      {
        message: 'Expected "}"',
        position: { line: 8, column: 12, lineText: 'const a = {;', length: 1 },
      },
      {
        message: 'Unexpected }',
        position: { line: 10, column: 11, lineText: 'const d = };', length: 1 },
      },
    ]
    const agg = new AggregateError(errors, 'Parse error')
    const result = formatSyntaxError(agg, 6)
    expect(result).toContain('Syntax errors (2 diagnostics):')
    expect(result).toContain('line 2,')
    expect(result).toContain('line 4,')
  })

  test('Error 对象（有 message 属性）走 BuildMessage 路径', () => {
    const result = formatSyntaxError(new Error('some error'), 0)
    expect(result).toContain('Syntax error (1 diagnostic):')
    expect(result).toContain('some error')
  })

  test('非 Error 对象回退到 String() 转换', () => {
    const result = formatSyntaxError('raw string', 0)
    expect(result).toBe('Syntax error: raw string')
  })

  test('caret 长度大于 1 时正确渲染', () => {
    const error: BuildMessageLike = {
      message: 'Unexpected token',
      position: { line: 7, column: 5, lineText: 'const abc = xyz', length: 3 },
    }
    const result = formatSyntaxError(error, 6)
    expect(result).toContain('    ^^^')
  })
})

// ---------- formatSyntaxError (real Bun.Transpiler end-to-end) ----------

describe('formatSyntaxError with real Bun.Transpiler', () => {
  const PREAMBLE_OFFSET = 6

  test('单行单个语法错误', () => {
    const error = catchTranspileError(
      'async function __script__(__ctx) {\n' +
        'const Read = __ctx["Read"];\n' +
        'const Write = __ctx["Write"];\n' +
        'const Edit = __ctx["Edit"];\n' +
        'const console = __ctx.console;\n' +
        'const utils = __ctx.utils;\n' +
        'const x: number = ;\n' +
        '}',
    )
    expect(error).not.toBeNull()
    const result = formatSyntaxError(error!, PREAMBLE_OFFSET)
    expect(result).toContain('line 1,')
    expect(result).toContain('Unexpected ;')
    expect(result).toContain('^')
  })

  test('多行中间语法错误行号修正正确', () => {
    const error = catchTranspileError(
      'async function __script__(__ctx) {\n' +
        'const Read = __ctx["Read"];\n' +
        'const Write = __ctx["Write"];\n' +
        'const Edit = __ctx["Edit"];\n' +
        'const console = __ctx.console;\n' +
        'const utils = __ctx.utils;\n' +
        'const a = 1\n' +
        'const b = 2\n' +
        'const x: number = ;\n' +
        'const c = 3\n' +
        '}',
    )
    expect(error).not.toBeNull()
    const result = formatSyntaxError(error!, PREAMBLE_OFFSET)
    expect(result).toContain('line 3,')
  })

  test('多错误场景（AggregateError）', () => {
    const error = catchTranspileError(
      'async function __script__(__ctx) {\n' +
        'const Read = __ctx["Read"];\n' +
        'const Write = __ctx["Write"];\n' +
        'const Edit = __ctx["Edit"];\n' +
        'const console = __ctx.console;\n' +
        'const utils = __ctx.utils;\n' +
        'const a = {; const b = };\n' +
        '}',
    )
    expect(error).not.toBeNull()
    const result = formatSyntaxError(error!, PREAMBLE_OFFSET)
    expect(result).toContain('diagnostics):')
    const lineMatches = result.match(/line \d+/g) ?? []
    expect(lineMatches.length).toBeGreaterThanOrEqual(2)
    for (const m of lineMatches) {
      const num = parseInt(m.replace('line ', ''), 10)
      expect(num).toBeGreaterThanOrEqual(1)
    }
  })

  test('import 语句报语法错误', () => {
    const error = catchTranspileError(
      'async function __script__(__ctx) {\n' +
        'const Read = __ctx["Read"];\n' +
        'const Write = __ctx["Write"];\n' +
        'const Edit = __ctx["Edit"];\n' +
        'const console = __ctx.console;\n' +
        'const utils = __ctx.utils;\n' +
        'import { readFileSync } from "fs"\n' +
        '}',
    )
    expect(error).not.toBeNull()
    const result = formatSyntaxError(error!, PREAMBLE_OFFSET)
    expect(result).toContain('line 1,')
  })
})

// ---------- formatTypeCheckFailure ----------

describe('formatTypeCheckFailure', () => {
  test('格式化包含错误诊断的结果', () => {
    const result: ScriptTypeCheckResult = {
      passed: false,
      durationMs: 150,
      errorCount: 2,
      warningCount: 0,
      totalDiagnosticCount: 2,
      diagnostics: [
        {
          severity: 'error',
          code: 2322,
          message: "Type 'string' is not assignable to type 'number'.",
          line: 3,
          column: 7,
        },
        {
          severity: 'error',
          code: 2304,
          message: "Cannot find name 'foo'.",
          line: 8,
          column: 5,
        },
      ],
      truncated: false,
    }
    const output = formatTypeCheckFailure(result)
    expect(output).toContain('Type check failed with 2 error(s) in 150ms.')
    expect(output).toContain('line 3, col 7 TS2322:')
    expect(output).toContain('line 8, col 5 TS2304:')
  })

  test('只展示 error 级别诊断，忽略 warning', () => {
    const result: ScriptTypeCheckResult = {
      passed: false,
      durationMs: 100,
      errorCount: 1,
      warningCount: 1,
      totalDiagnosticCount: 2,
      diagnostics: [
        {
          severity: 'error',
          code: 2322,
          message: 'type error',
          line: 1,
          column: 1,
        },
        {
          severity: 'warning',
          code: 6133,
          message: 'unused var',
          line: 2,
          column: 5,
        },
      ],
      truncated: false,
    }
    const output = formatTypeCheckFailure(result)
    expect(output).toContain('TS2322')
    expect(output).not.toContain('TS6133')
  })

  test('截断时显示剩余诊断计数', () => {
    const result: ScriptTypeCheckResult = {
      passed: false,
      durationMs: 200,
      errorCount: 25,
      warningCount: 0,
      totalDiagnosticCount: 25,
      diagnostics: [
        {
          severity: 'error',
          code: 2304,
          message: "Cannot find name 'x'.",
          line: 1,
          column: 1,
        },
      ],
      truncated: true,
    }
    const output = formatTypeCheckFailure(result)
    expect(output).toContain('... and 24 more diagnostic(s).')
  })

  test('截断但无剩余时不显示额外提示', () => {
    const result: ScriptTypeCheckResult = {
      passed: false,
      durationMs: 50,
      errorCount: 1,
      warningCount: 0,
      totalDiagnosticCount: 1,
      diagnostics: [
        {
          severity: 'error',
          code: 2304,
          message: 'err',
          line: 1,
          column: 1,
        },
      ],
      truncated: true,
    }
    const output = formatTypeCheckFailure(result)
    expect(output).not.toContain('more diagnostic')
  })
})
