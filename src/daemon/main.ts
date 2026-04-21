export async function daemonMain(_args: string[]): Promise<void> {
  throw new Error('Daemon mode is unavailable in this source snapshot.')
}
