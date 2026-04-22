/**
 * Source-restore fallback:
 * some internal builds provide a live monitor component for Tungsten.
 * In this workspace the implementation is unavailable, so expose a no-op
 * component to keep interactive REPL startup functional.
 */
export function TungstenLiveMonitor(): null {
  return null;
}
