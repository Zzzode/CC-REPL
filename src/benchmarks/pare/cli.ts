import { readFile } from 'node:fs/promises'
import { runBenchmark } from './run.js'

type ParsedArgs = {
  configPath?: string
  cases: string
  model?: string
  permissionMode: string
  maxCases?: number
  failOnRegression: boolean
  runsPerCase: number
  baselineLabel: string
  baselineCommand: string
  baselineArgs: string[]
  candidateLabel: string
  candidateCommand: string
  candidateArgs: string[]
  workspaceMode: 'tmp-git' | 'worktree' | 'current'
}

type BenchmarkConfig = {
  cases?: string
  model?: string
  permissionMode?: string
  maxCases?: number
  failOnRegression?: boolean
  runsPerCase?: number
  workspaceMode?: 'tmp-git' | 'worktree' | 'current'
  baseline?: { label?: string; command?: string; args?: string[] }
  candidate?: { label?: string; command?: string; args?: string[] }
}

const defaultParsedArgs: ParsedArgs = {
  cases: 'benchmarks/pare/cases/core.json',
  permissionMode: 'bypassPermissions',
  failOnRegression: false,
  runsPerCase: 1,
  baselineLabel: 'baseline',
  baselineCommand: 'bun',
  baselineArgs: ['dist/cli.js'],
  candidateLabel: 'candidate',
  candidateCommand: 'bun',
  candidateArgs: ['dist/cli.js'],
  workspaceMode: 'tmp-git',
}

async function loadConfig(path: string): Promise<BenchmarkConfig> {
  const raw = await readFile(path, 'utf8')
  const parsed = JSON.parse(raw)
  if (!parsed || typeof parsed !== 'object') {
    throw new Error('Invalid benchmark config: expected JSON object')
  }
  return parsed as BenchmarkConfig
}

function applyConfig(base: ParsedArgs, config: BenchmarkConfig): ParsedArgs {
  const merged: ParsedArgs = {
    ...base,
    cases: typeof config.cases === 'string' ? config.cases : base.cases,
    model: typeof config.model === 'string' ? config.model : base.model,
    permissionMode:
      typeof config.permissionMode === 'string'
        ? config.permissionMode
        : base.permissionMode,
    maxCases:
      typeof config.maxCases === 'number' && Number.isFinite(config.maxCases)
        ? config.maxCases
        : base.maxCases,
    failOnRegression:
      typeof config.failOnRegression === 'boolean'
        ? config.failOnRegression
        : base.failOnRegression,
    runsPerCase:
      typeof config.runsPerCase === 'number' && Number.isFinite(config.runsPerCase)
        ? Math.max(1, Math.floor(config.runsPerCase))
        : base.runsPerCase,
    workspaceMode:
      config.workspaceMode === 'tmp-git' ||
      config.workspaceMode === 'worktree' ||
      config.workspaceMode === 'current'
        ? config.workspaceMode
        : base.workspaceMode,
    baselineLabel:
      typeof config.baseline?.label === 'string'
        ? config.baseline.label
        : base.baselineLabel,
    baselineCommand:
      typeof config.baseline?.command === 'string'
        ? config.baseline.command
        : base.baselineCommand,
    baselineArgs:
      Array.isArray(config.baseline?.args) &&
      config.baseline?.args.every((v) => typeof v === 'string')
        ? config.baseline.args
        : base.baselineArgs,
    candidateLabel:
      typeof config.candidate?.label === 'string'
        ? config.candidate.label
        : base.candidateLabel,
    candidateCommand:
      typeof config.candidate?.command === 'string'
        ? config.candidate.command
        : base.candidateCommand,
    candidateArgs:
      Array.isArray(config.candidate?.args) &&
      config.candidate?.args.every((v) => typeof v === 'string')
        ? config.candidate.args
        : base.candidateArgs,
  }
  return merged
}

async function parseArgs(argv: string[]): Promise<ParsedArgs> {
  let configPath: string | undefined
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === '--config' && argv[i + 1]) {
      configPath = argv[i + 1]
      i += 1
    }
  }

  let parsed: ParsedArgs = { ...defaultParsedArgs }
  if (configPath) {
    parsed = {
      ...applyConfig(parsed, await loadConfig(configPath)),
      configPath,
    }
  }

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i]
    const next = argv[i + 1]

    if (arg === '--config' && next) {
      parsed.configPath = next
      i += 1
      continue
    }

    if (arg === '--cases' && next) {
      parsed.cases = next
      i += 1
      continue
    }
    if (arg === '--model' && next) {
      parsed.model = next
      i += 1
      continue
    }
    if (arg === '--permission-mode' && next) {
      parsed.permissionMode = next
      i += 1
      continue
    }
    if (arg === '--max-cases' && next) {
      parsed.maxCases = Number(next)
      i += 1
      continue
    }
    if (arg === '--fail-on-regression') {
      parsed.failOnRegression = true
      continue
    }
    if (arg === '--runs-per-case' && next) {
      const value = Number(next)
      if (Number.isFinite(value) && value >= 1) {
        parsed.runsPerCase = Math.floor(value)
      }
      i += 1
      continue
    }
    if (arg === '--baseline-label' && next) {
      parsed.baselineLabel = next
      i += 1
      continue
    }
    if (arg === '--baseline-cmd' && next) {
      parsed.baselineCommand = next
      i += 1
      continue
    }
    if (arg === '--baseline-args' && next) {
      parsed.baselineArgs = next.split(' ').filter(Boolean)
      i += 1
      continue
    }
    if (arg === '--candidate-label' && next) {
      parsed.candidateLabel = next
      i += 1
      continue
    }
    if (arg === '--candidate-cmd' && next) {
      parsed.candidateCommand = next
      i += 1
      continue
    }
    if (arg === '--candidate-args' && next) {
      parsed.candidateArgs = next.split(' ').filter(Boolean)
      i += 1
      continue
    }
    if (arg === '--workspace-mode' && next) {
      if (next === 'tmp-git' || next === 'worktree' || next === 'current') {
        parsed.workspaceMode = next
      }
      i += 1
      continue
    }
    if (arg === '--help') {
      console.log('Usage: bun run src/benchmarks/pare/cli.ts [options]\n\nOptions:\n  --config <path>\n  --cases <path>\n  --model <model>\n  --permission-mode <mode>              (default: bypassPermissions)\n  --max-cases <n>\n  --fail-on-regression\n  --runs-per-case <n>                   (default: 1)\n  --baseline-label <name>\n  --baseline-cmd <command>              (default: bun)\n  --baseline-args "<args>"              (default: dist/cli.js)\n  --candidate-label <name>\n  --candidate-cmd <command>             (default: bun)\n  --candidate-args "<args>"             (default: dist/cli.js)\n  --workspace-mode <tmp-git|worktree|current>   (default: tmp-git)\n')
      process.exit(0)
    }
  }

  return parsed
}

function formatGrouped(grouped: { total: number; pass: number; tokens: number }): string {
  const passRate = grouped.total === 0 ? 0 : (grouped.pass / grouped.total) * 100
  return 'count=' + grouped.total + ' pass=' + passRate.toFixed(1) + '% tokens=' + grouped.tokens
}

async function main() {
  const parsed = await parseArgs(process.argv.slice(2))
  const { comparison, artifactDir } = await runBenchmark({
    casesPath: parsed.cases,
    model: parsed.model,
    permissionMode: parsed.permissionMode,
    maxCases: parsed.maxCases,
    failOnRegression: parsed.failOnRegression,
    runsPerCase: parsed.runsPerCase,
    baseline: {
      label: parsed.baselineLabel,
      command: parsed.baselineCommand,
      args: parsed.baselineArgs,
    },
    candidate: {
      label: parsed.candidateLabel,
      command: parsed.candidateCommand,
      args: parsed.candidateArgs,
    },
    workspaceMode: parsed.workspaceMode,
  })

  const base = comparison.aggregate.baseline
  const cand = comparison.aggregate.candidate
  const delta = comparison.aggregate.delta
  console.log('Pare benchmark finished\n')
  console.log('Artifacts: ' + artifactDir)
  console.log('Cases: ' + base.totalCount)
  console.log('Pass rate: baseline=' + (base.passRate * 100).toFixed(2) + '% candidate=' + (cand.passRate * 100).toFixed(2) + '% delta=' + (delta.passRate * 100).toFixed(2) + '%')
  console.log('Execution errors: baseline=' + base.errorCount + ' (' + (base.errorRate * 100).toFixed(2) + '%) candidate=' + cand.errorCount + ' (' + (cand.errorRate * 100).toFixed(2) + '%) delta=' + (cand.errorCount - base.errorCount))
  if (comparison.aggregate.v2) {
    const v2 = comparison.aggregate.v2
    console.log('V2 metrics: runsPerCase=' + v2.runsPerCase + ' evaluated=' + v2.evaluatedCases + ' skipped=' + v2.skippedCases + ' unavailable=' + v2.unavailableCases + ' weightedTokenReduction=' + (v2.weightedTokenReductionPct * 100).toFixed(2) + '%')
  }
  if (cand.errorCount > 0) {
    console.log('Top candidate errors:')
    for (const item of cand.topErrors) {
      console.log('- ' + item.count + 'x ' + item.message)
    }
  }
  console.log('Total tokens: baseline=' + base.tokens.totalTokens + ' candidate=' + cand.tokens.totalTokens + ' delta=' + delta.totalTokens + ' (' + (delta.totalTokensPct * 100).toFixed(2) + '%)')
  console.log('Total duration: baseline=' + base.totalDurationMs.toFixed(0) + 'ms candidate=' + cand.totalDurationMs.toFixed(0) + 'ms delta=' + delta.totalDurationMs.toFixed(0) + 'ms (' + (delta.totalDurationPct * 100).toFixed(2) + '%)')
  console.log('Avg duration: baseline=' + base.avgDurationMs.toFixed(2) + 'ms candidate=' + cand.avgDurationMs.toFixed(2) + 'ms delta=' + delta.avgDurationMs.toFixed(2) + 'ms (' + (delta.avgDurationPct * 100).toFixed(2) + '%)')

  const categories = Object.keys(comparison.grouped.candidate.byCategory).sort()
  if (categories.length > 0) {
    console.log('\nGrouped by category (candidate):')
    for (const c of categories) {
      console.log('- ' + c + ': ' + formatGrouped(comparison.grouped.candidate.byCategory[c]))
    }
  }

  const freq = Object.keys(comparison.grouped.candidate.byFrequency).sort()
  if (freq.length > 0) {
    console.log('\nGrouped by frequency (candidate):')
    for (const f of freq) {
      console.log('- ' + f + ': ' + formatGrouped(comparison.grouped.candidate.byFrequency[f]))
    }
  }
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error))
  process.exit(1)
})
