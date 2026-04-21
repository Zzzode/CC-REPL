const SENTINELS = new Map([
  ['com.apple.Terminal', 'shell'],
  ['com.googlecode.iterm2', 'shell'],
  ['com.apple.finder', 'filesystem'],
  ['com.apple.systempreferences', 'system_settings'],
  ['com.apple.SystemSettings', 'system_settings'],
])

export function getSentinelCategory(bundleId) {
  if (typeof bundleId !== 'string' || bundleId.length === 0) {
    return null
  }
  return SENTINELS.get(bundleId) ?? null
}
