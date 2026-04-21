const unavailable = () => {
  throw new Error(
    '@ant/computer-use-swift is unavailable in this build. Install the private package to enable Computer Use.',
  )
}

module.exports = {
  tcc: {
    checkAccessibility: () => false,
    checkScreenRecording: () => false,
  },
  hotkey: {
    registerEscape: () => false,
    unregister: () => {},
    notifyExpectedEscape: () => {},
  },
  display: {
    getSize: () => ({
      width: 1440,
      height: 900,
      scaleFactor: 2,
      displayId: 0,
      originX: 0,
      originY: 0,
    }),
    listAll: () => [
      {
        width: 1440,
        height: 900,
        scaleFactor: 2,
        displayId: 0,
        originX: 0,
        originY: 0,
      },
    ],
  },
  apps: {
    prepareDisplay: async () => ({ hidden: [], activated: null }),
    previewHideSet: async () => [],
    findWindowDisplays: async () => [],
    appUnderPoint: async () => null,
    listInstalled: async () => [],
    iconDataUrl: () => null,
    listRunning: async () => [],
    open: async () => unavailable(),
    unhide: async () => {},
  },
  screenshot: {
    captureExcluding: async () => ({
      base64: '',
      width: 1,
      height: 1,
      displayWidth: 1,
      displayHeight: 1,
      displayId: 0,
      originX: 0,
      originY: 0,
    }),
    captureRegion: async () => ({
      base64: '',
      width: 1,
      height: 1,
    }),
  },
  resolvePrepareCapture: async () => ({
    hidden: [],
    screenshot: {
      base64: '',
      width: 1,
      height: 1,
      displayWidth: 1,
      displayHeight: 1,
      displayId: 0,
      originX: 0,
      originY: 0,
    },
    resolvedDisplayId: 0,
  }),
}
