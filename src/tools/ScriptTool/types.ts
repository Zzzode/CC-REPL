export interface ScriptToolInput {
  code: string
  description?: string
  timeout_ms?: number
}

export interface ScriptToolOutput {
  result: unknown
  stdout: string
  stderr: string
  duration_ms: number
  timed_out: boolean
}
