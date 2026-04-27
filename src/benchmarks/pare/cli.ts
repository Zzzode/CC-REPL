import { runBenchmark } from './run.js'

type ParsedArgs = {
  cases: string
  model?: string
  permissionMode: string
  maxCases?: number
  failOnRegression: boolean
  baselineLabel: string
  baselineCommand: string
  baselineArgs: string[]
  candidateLabel: string
  candidateCommand: string
  candidateArgs: string[]
}

function parseArgs(argv: string[]): ParsedArgs {
  const parsed: ParsedArgs = {
    cases: 'benchmarks/pare/cases/core.json',
    permissionMode: 'bypassPermissions',
    failOnRegression: false,
    baselineLabel: 'baseline',
    baselineCommand: 'bun',
    baselineArgs: ['dist/cli.js'],
    candidateLabel: 'candidate',
    candidateCommand: 'bun',
    candidateArgs: ['dist/cli.js'],
  }

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i]
    const next = argv[i + 1]

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
    if (arg === '--help') {
      console.log('Usage: bun run src/benchmarks/pare/cli.ts [options]\n\nOptions:\n  --cases <path>\n  --model <model>\n  --permission-mode <mode>              (default: bypassPermissions)\n  --max-cases <n>\n  --fail-on-regression\n  --baseline-label <name>\n  --baseline-cmd <command>              (default: bun)\n  --baseline-args "<args>"              (default: dist/cli.js)\n  --candidate-label <name>\n  --candidate-cmd <command>             (default: bun)\n  --candidate-args "<args>"             (default: dist/cli.js)\n')
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
  const parsed = parseArgs(process.argv.slice(2))
  const { comparison, artifactDir } = await runBenchmark({
    casesPath: parsed.cases,
    model: parsed.model,
    permissionMode: parsed.permissionMode,
    maxCases: parsed.maxCases,
    failOnRegression: parsed.failOnRegression,
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
  })

  const base = comparison.aggregate.baseline
  const cand = comparison.aggregate.candidate
  const delta = comparison.aggregate.delta
  console.log('Pare benchmark finished\n')
  console.log('Artifacts: ' + artifactDir)
  console.log('Cases: ' + base.totalCount)
  console.log('Pass rate: baseline=' + (base.passRate * 100).toFixed(2) + '% candidate=' + (cand.passRate * 100).toFixed(2) + '% delta=' + (delta.passRate * 100).toFixed(2) + '%')
  console.log('Total tokens: baseline=' + base.tokens.totalTokens + ' candidate=' + cand.tokens.totalTokens + ' delta=' + delta.totalTokens + ' (' + (delta.totalTokensPct * 100).toFixed(2) + '%)')

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
