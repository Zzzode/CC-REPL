export type DisplayGeometry = {
  width: number
  height: number
  scaleFactor: number
  displayId?: number
  originX?: number
  originY?: number
}

export interface ComputerUseAPI {
  tcc: {
    checkAccessibility(): boolean
    checkScreenRecording(): boolean
  }
  hotkey: {
    registerEscape(handler: () => void): boolean
    unregister(): void
    notifyExpectedEscape(): void
  }
  display: {
    getSize(displayId?: number): DisplayGeometry
    listAll(): DisplayGeometry[]
  }
  apps: {
    prepareDisplay(
      allowlistBundleIds: string[],
      hostBundleId: string,
      displayId?: number,
    ): Promise<{ hidden: string[]; activated: string | null }>
    previewHideSet(
      allowlistBundleIds: string[],
      displayId?: number,
    ): Promise<Array<{ bundleId: string; displayName: string }>>
    findWindowDisplays(
      bundleIds: string[],
    ): Promise<Array<{ bundleId: string; displayIds: number[] }>>
    appUnderPoint(
      x: number,
      y: number,
    ): Promise<{ bundleId: string; displayName: string } | null>
    listInstalled(): Promise<Array<{ bundleId: string; displayName: string; path: string }>>
    iconDataUrl(path: string): string | null
    listRunning(): Promise<Array<{ bundleId: string; displayName: string }>>
    open(bundleId: string): Promise<void>
    unhide(bundleIds: string[]): Promise<void>
  }
  screenshot: {
    captureExcluding(
      allowedBundleIds: string[],
      jpegQuality: number,
      width: number,
      height: number,
      displayId?: number,
    ): Promise<{
      base64: string
      width: number
      height: number
      displayWidth: number
      displayHeight: number
      displayId?: number
      originX?: number
      originY?: number
    }>
    captureRegion(
      allowedBundleIds: string[],
      x: number,
      y: number,
      width: number,
      height: number,
      outWidth: number,
      outHeight: number,
      jpegQuality: number,
      displayId?: number,
    ): Promise<{ base64: string; width: number; height: number }>
  }
  resolvePrepareCapture(
    allowedBundleIds: string[],
    hostBundleId: string,
    jpegQuality: number,
    targetWidth: number,
    targetHeight: number,
    preferredDisplayId?: number,
    autoResolve?: boolean,
    doHide?: boolean,
  ): Promise<{
    hidden: string[]
    screenshot: {
      base64: string
      width: number
      height: number
      displayWidth: number
      displayHeight: number
      displayId?: number
      originX?: number
      originY?: number
    }
    resolvedDisplayId?: number
  }>
}
