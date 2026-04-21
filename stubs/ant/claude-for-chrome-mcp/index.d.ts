export type PermissionMode =
  | 'ask'
  | 'skip_all_permission_checks'
  | 'follow_a_plan'

export type Logger = {
  silly(message: string, ...args: unknown[]): void
  debug(message: string, ...args: unknown[]): void
  info(message: string, ...args: unknown[]): void
  warn(message: string, ...args: unknown[]): void
  error(message: string, ...args: unknown[]): void
}

export type ClaudeForChromeContext = {
  serverName: string
  logger: Logger
  socketPath: string
  getSocketPaths: () => string[]
  clientTypeId: string
  onAuthenticationError?: () => void
  onToolCallDisconnected?: () => string
  onExtensionPaired?: (deviceId: string, name: string) => void
  getPersistedDeviceId?: () => string | undefined
  bridgeConfig?: {
    url: string
    getUserId?: () => Promise<string | undefined>
    getOAuthToken?: () => Promise<string>
    devUserId?: string
  }
  initialPermissionMode?: PermissionMode
  callAnthropicMessages?: (req: unknown) => Promise<unknown>
  trackEvent?: (eventName: string, metadata?: Record<string, unknown>) => void
}

export const BROWSER_TOOLS: ReadonlyArray<{ name: string }>

export function createClaudeForChromeMcpServer(
  context: ClaudeForChromeContext,
): {
  setRequestHandler(schema: unknown, handler: (...args: unknown[]) => unknown): void
  connect(transport: unknown): Promise<void>
  close(): Promise<void>
}
