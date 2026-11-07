export interface GraphicsCapabilities {
  egl: boolean;
  gles3: boolean;
  nativeVsync: boolean;
  zeroCopyPresentation: boolean;
  desktopGl: boolean;
  vulkan: boolean;
  shmDiagnostic: boolean;
}

export const startServer: (socketPath: string) => boolean;
export const stopServer: () => void;
export const setStateCallback: (callback: (state: string) => void) => void;
export const setToplevelCallback: (callback: (id: number, event: string, json: string) => void) => void;
export const createRenderer: (surfaceId: bigint) => void;
export const resizeRenderer: (width: number, height: number) => void;
export const destroyRenderer: () => void;
export const bindToplevel: (id: number) => void;
export const setOutputSize: (width: number, height: number) => void;
export const setContentInsets: (left: number, top: number, right: number, bottom: number) => void;
export const setOutputScale: (scale: number) => void;
export const sendPointerEvent: (action: number, x: number, y: number, button: number,
  rawDeltaX?: number, rawDeltaY?: number, fromMouse?: boolean) => void;
export const sendKeyEvent: (evdevCode: number, pressed: boolean) => void;
export const sendScrollEvent: (axis: number, value: number, scrollStep: number,
  x: number, y: number, source: number, stop: boolean) => void;
export const watchCoasting: (content: object,
  callback: (phase: number, deltaX: number, deltaY: number) => void) => number;
export const getGraphicsCapabilities: () => GraphicsCapabilities;
// The diagnostics overlay's numbers, all measured on the presentation path.
export interface Diagnostics {
  // Client frames of the bound toplevel presented in the last second.
  fps: number;
  // Client frames presented since the stats began.
  presented: number;
  refreshHz: number;
  // Mean and longest gap between those frames; 0 with fewer than two.
  frameMs: number;
  worstFrameMs: number;
  // Every gap between those frames, oldest first.
  intervalsMs: number[];
  // Last key, button, or scroll to the next presented client frame; -1 if none.
  inputMs: number;
  // "gpu" (direct NativeImage), "shm" (copied wl_shm), or "" before a frame.
  path: string;
  bufferWidth: number;
  bufferHeight: number;
  // Client buffer pixels per logical pixel.
  clientScale: number;
  // View pixels per client buffer pixel: 1 is pixel exact.
  pixelRatio: number;
  // Resident memory of process pid; -1 if unreadable.
  rssBytes: number;
}
export const getDiagnostics: (pid: number) => Diagnostics;
export const startGpuHost: (runtimeDir: string) => boolean;
export const stopGpuHost: () => void;
export const getGpuGuestEnvironment: () => string[];
export const launchChild: (appId: string, packagePath: string, entryPoint: string,
  args: string[], env: string[]) => number;
export const stopChild: (appId: string) => void;
export interface AppRecord {
  appId: string;
  packagePath: string;
  entryPoint: string;
}
export const getApps: () => AppRecord[];
export const registerApp: (appId: string, packagePath: string, entryPoint: string,
  args: string[], env: string[]) => void;
export const probePty: () => number;
export const ensureSshKey: (directory: string) => string;
export const sendText: (text: string) => void;
// Unpacks a rawfile tar (getRawFd) into dest; entries written, or -errno.
// Diagnostic: writes the next composed frame to path as a binary PPM.
// The HarmonyOS window was maximized (or full screen) or restored.
export const setToplevelMaximized: (maximized: boolean) => void;
export const closeToplevel: () => boolean;
export const activateTitlebar: (json: string) => void;
export const startSessionBus: (socketPath: string,
  callback: (id: number, kind: string, json: string) => void) => boolean;
export const portalRespond: (id: number, response: number, uris: string[]) => void;
export const setPasteboardCallback: (callback: () => void) => void;
export const pasteboardPermission: (granted: boolean) => void;
export const dragEvent: (kind: number, x: number, y: number, mimeTypes: string[], payloads: string[]) => void;
export const presentedFps: () => number;
export const captureFrame: (path: string) => void;
export const extractTar: (fd: number, offset: number, length: number, dest: string) => number;
