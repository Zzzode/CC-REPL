export type UpstreamScenarioType = 'reproducible' | 'mutating' | 'unknown'

export type UpstreamScenario = {
  pareScenarioId: string
  pareScenarioName: string
  category: string
  type: UpstreamScenarioType
  frequency: string
  sourceFile: string
}

export type LocalScenario = {
  caseId: string
  pareScenarioId: string
  pareScenarioName: string
  category: string
  type: UpstreamScenarioType
  frequency: string
  sourceFile: string
}

export const PARE_V2_CSV_REPRO =
  'https://raw.githubusercontent.com/Dave-London/Pare/main/benchmarks/v2/data/Benchmark-Detailed.csv'
export const PARE_V2_CSV_MUT =
  'https://raw.githubusercontent.com/Dave-London/Pare/main/benchmarks/v2/data/latest-mutating-results.csv'
