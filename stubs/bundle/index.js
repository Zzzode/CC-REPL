const enabledFromEnv = new Set(
  (process.env.CLAUDE_CODE_FEATURES ?? '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean),
)

export function feature(name) {
  if (enabledFromEnv.has(name)) {
    return true
  }
  return false
}
