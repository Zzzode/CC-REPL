function unavailable(): never {
  throw new Error('Background session commands are unavailable in this source snapshot.')
}

export async function psHandler(_args: string[]): Promise<void> {
  unavailable()
}

export async function logsHandler(_sessionId?: string): Promise<void> {
  unavailable()
}

export async function attachHandler(_sessionId?: string): Promise<void> {
  unavailable()
}

export async function killHandler(_sessionId?: string): Promise<void> {
  unavailable()
}

export async function handleBgFlag(_args: string[]): Promise<void> {
  unavailable()
}
