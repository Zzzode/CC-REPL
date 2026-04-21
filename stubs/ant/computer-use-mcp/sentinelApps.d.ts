export type SentinelCategory = 'shell' | 'filesystem' | 'system_settings'

export function getSentinelCategory(
  bundleId: string,
): SentinelCategory | null
