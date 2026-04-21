export type CoordinateMode = 'pixels' | 'normalized'

export type CuSubGates = {
  pixelValidation: boolean
  clipboardPasteMultiline: boolean
  mouseAnimation: boolean
  hideBeforeAction: boolean
  autoTargetDisplay: boolean
  clipboardGuard: boolean
}

export type Logger = {
  silly(message: string, ...args: unknown[]): void
  debug(message: string, ...args: unknown[]): void
  info(message: string, ...args: unknown[]): void
  warn(message: string, ...args: unknown[]): void
  error(message: string, ...args: unknown[]): void
}

export type DisplayGeometry = {
  width: number
  height: number
  scaleFactor: number
  displayId?: number
  originX?: number
  originY?: number
}

export type FrontmostApp = {
  bundleId: string
  displayName: string
}

export type InstalledApp = {
  bundleId: string
  displayName: string
  path?: string
  iconDataUrl?: string
}

export type RunningApp = {
  bundleId: string
  displayName: string
}

export type ScreenshotResult = {
  base64: string
  width: number
  height: number
  displayWidth: number
  displayHeight: number
  displayId?: number
  originX?: number
  originY?: number
}

export type ResolvePrepareCaptureResult = {
  hidden: string[]
  screenshot: ScreenshotResult
  resolvedDisplayId?: number
}

export type ComputerExecutor = {
  capabilities: Record<string, unknown>
  prepareForAction(allowlistBundleIds: string[], displayId?: number): Promise<string[]>
  previewHideSet(
    allowlistBundleIds: string[],
    displayId?: number,
  ): Promise<Array<{ bundleId: string; displayName: string }>>
  getDisplaySize(displayId?: number): Promise<DisplayGeometry>
  listDisplays(): Promise<DisplayGeometry[]>
  findWindowDisplays(
    bundleIds: string[],
  ): Promise<Array<{ bundleId: string; displayIds: number[] }>>
  resolvePrepareCapture(opts: {
    allowedBundleIds: string[]
    preferredDisplayId?: number
    autoResolve: boolean
    doHide?: boolean
  }): Promise<ResolvePrepareCaptureResult>
  screenshot(opts: { allowedBundleIds: string[]; displayId?: number }): Promise<ScreenshotResult>
  zoom(
    regionLogical: { x: number; y: number; w: number; h: number },
    allowedBundleIds: string[],
    displayId?: number,
  ): Promise<{ base64: string; width: number; height: number }>
  key(keySequence: string, repeat?: number): Promise<void>
  holdKey(keyNames: string[], durationMs: number): Promise<void>
  type(text: string, opts: { viaClipboard: boolean }): Promise<void>
  readClipboard(): Promise<string>
  writeClipboard(text: string): Promise<void>
  moveMouse(x: number, y: number): Promise<void>
  click(
    x: number,
    y: number,
    button: 'left' | 'right' | 'middle',
    count: 1 | 2 | 3,
    modifiers?: string[],
  ): Promise<void>
  mouseDown(): Promise<void>
  mouseUp(): Promise<void>
  getCursorPosition(): Promise<{ x: number; y: number }>
  drag(from: { x: number; y: number } | undefined, to: { x: number; y: number }): Promise<void>
  scroll(x: number, y: number, dx: number, dy: number): Promise<void>
  getFrontmostApp(): Promise<FrontmostApp | null>
  appUnderPoint(
    x: number,
    y: number,
  ): Promise<{ bundleId: string; displayName: string } | null>
  listInstalledApps(): Promise<InstalledApp[]>
  getAppIcon(path: string): Promise<string | undefined>
  listRunningApps(): Promise<RunningApp[]>
  openApp(bundleId: string): Promise<void>
}

export type ComputerUseHostAdapter = {
  serverName: string
  logger: Logger
  executor: ComputerExecutor
  ensureOsPermissions: () =>
    | Promise<{ granted: true }>
    | Promise<{
        granted: false
        accessibility: boolean
        screenRecording: boolean
      }>
  isDisabled: () => boolean
  getSubGates: () => CuSubGates
  getAutoUnhideEnabled: () => boolean
  cropRawPatch: (...args: unknown[]) => unknown
}

export type AppGrant = {
  bundleId: string
  displayName: string
  grantedAt: number
}

export type CuGrantFlags = {
  clipboardRead: boolean
  clipboardWrite: boolean
  systemKeyCombos: boolean
}

export type ScreenshotDims = {
  width: number
  height: number
  displayWidth: number
  displayHeight: number
  displayId?: number
  originX?: number
  originY?: number
}

export type CuPermissionRequest = {
  apps: Array<{
    requestedName: string
    resolved?: {
      bundleId: string
      displayName: string
    }
    alreadyGranted?: boolean
  }>
  reason?: string
  requestedFlags: CuGrantFlags
  tccState?: {
    accessibility: boolean
    screenRecording: boolean
  }
}

export type CuPermissionResponse = {
  granted: AppGrant[]
  denied: string[]
  flags: CuGrantFlags
}

export type CuCallToolResult = {
  content:
    | string
    | Array<
        | { type: 'text'; text: string }
        | { type: 'image'; data: string; mimeType?: string }
      >
  isError?: boolean
  telemetry?: {
    error_kind?: string
  }
}

export type ComputerUseSessionContext = {
  getAllowedApps: () => readonly AppGrant[]
  getGrantFlags: () => CuGrantFlags
  getUserDeniedBundleIds: () => readonly string[]
  getSelectedDisplayId: () => number | undefined
  getDisplayPinnedByModel: () => boolean
  getDisplayResolvedForApps: () => string | undefined
  getLastScreenshotDims: () => ScreenshotDims | undefined
  onPermissionRequest: (
    req: CuPermissionRequest,
    dialogSignal?: AbortSignal,
  ) => Promise<CuPermissionResponse>
  onAllowedAppsChanged: (apps: AppGrant[], flags: CuGrantFlags) => void
  onAppsHidden: (bundleIds: string[]) => void
  onResolvedDisplayUpdated: (displayId: number) => void
  onDisplayPinned: (displayId: number | undefined) => void
  onDisplayResolvedForApps: (appKey: string) => void
  onScreenshotCaptured: (dims: ScreenshotDims) => void
  checkCuLock: () => Promise<{ holder: string | undefined; isSelf: boolean }>
  acquireCuLock: () => Promise<void>
  formatLockHeldMessage: (holder: string) => string
}

export const DEFAULT_GRANT_FLAGS: CuGrantFlags
export const API_RESIZE_PARAMS: {
  maxWidth: number
  maxHeight: number
}

export function targetImageSize(
  width: number,
  height: number,
  params?: unknown,
): [number, number]

export function buildComputerUseTools(
  capabilities: unknown,
  coordinateMode: CoordinateMode,
  installedAppNames?: string[],
): Array<{
  name: string
  description?: string
  inputSchema?: Record<string, unknown>
}>

export function createComputerUseMcpServer(
  adapter: ComputerUseHostAdapter,
  coordinateMode: CoordinateMode,
): {
  setRequestHandler(schema: unknown, handler: (...args: unknown[]) => unknown): void
  connect(transport: unknown): Promise<void>
  close(): Promise<void>
}

export function bindSessionContext(
  adapter: ComputerUseHostAdapter,
  coordinateMode: CoordinateMode,
  ctx: ComputerUseSessionContext,
): (name: string, args: unknown) => Promise<CuCallToolResult>
