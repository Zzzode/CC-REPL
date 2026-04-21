function unavailable(kind?: string): never {
  const suffix = kind ? ` (${kind})` : ''
  throw new Error(`Daemon worker${suffix} is unavailable in this source snapshot.`)
}

export async function runDaemonWorker(kind?: string): Promise<void> {
  unavailable(kind)
}
