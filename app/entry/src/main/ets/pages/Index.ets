import { display, NodeContent, window } from '@kit.ArkUI';
import { abilityAccessCtrl, bundleManager, common, ConfigurationConstant, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { Environment, fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { buffer } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import runtime from 'libentry.so';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { TouchpadScroll } from '../common/TouchpadScroll';
import { DROP_TYPES, dragMimeTypes, dropPayload } from '../common/DragData';
import { EXIT_MODERATE_MS, SPRING_MODERATE } from '../common/Design';
import { CornerRuler } from '../common/CornerRuler';
import { CornerInsets, DisplayCorners, displayCorners, NO_CORNERS, NO_INSETS } from '../common/DisplayCorners';
import { titleBarCss } from '../common/TitleBarStyle';
import { iconName, NativeTitleBar, TitleBarItem, TitleBarModel } from '../common/NativeTitleBar';
import { DiagnosticsGallery, DiagnosticsOverlay, DiagnosticsSampler, GALLERY_STATES, GalleryState,
  Summary } from '../common/DiagnosticsOverlay';

// One program the HAP can run: a library in the HAP libs directory and the
// symbol to enter (an executable's `main` by default), or hnp:<name>, a
// stock executable from the HAP's native package.
interface AppEntry {
  exec: string;
  entry?: string;
  args?: string[];
  env?: string[];
  // false runs the program without the direct-GPU environment.
  gpu?: boolean;
  // How the native title bar shows in a maximized window: "fixed", the default,
  // keeps it at the top; "hover" overlays it while the status bar shows.
  titlebarMode?: string;
  // Testing HAPs: a recorded input file (fixtures/*.tsv) the launcher feeds
  // through the real input path once the program's window is up.
  replay?: string;
}

interface AppFile {
  path: string;
  text: string;
  // Write only when the file does not exist yet (seeded user settings).
  ifMissing?: boolean;
  // JSON settings the app needs on HarmonyOS: inserted into an existing
  // file unless it already names the key, so user settings are kept.
  ensureKeys?: Record<string, string>;
}

// rawfile/app.json, written by `ohos-wl hap`. Strings may use ${LIBS}, ${HNP},
// ${FILES}, ${CACHE}, ${HOME}, and ${RUNTIME_DIR}.
interface AppManifest extends AppEntry {
  name: string;
  // rawfile tars unpacked into ${FILES} when the HAP changes.
  data?: string[];
  // Text files written before every launch.
  files?: AppFile[];
  // Runtime services the app needs: "ssh-key" makes the HiShell login key.
  setup?: string[];
  // Testing HAPs: named programs for `aa start ... --ps test <name>`.
  tests?: Record<string, AppEntry>;
  defaultTest?: string;
  // "client": the app draws its own title bar, so the HarmonyOS one is
  // hidden from the start and the window never changes size at first frame.
  decorations?: string;
  // The app draws a bar along its top edge while it fills the screen, so that
  // bar reaches the screen edge in place of the native title bar.
  topBar?: boolean;
  // Native title bar placements by icon name: lead, afterTitle, center,
  // actions, or menu (NativeTitleBar slotOf has the defaults).
  titlebar?: Record<string, string>;
  // Title bar icon aliases by icon name, such as Ghostty's view-grid button,
  // which opens the tab overview (SymbolMap.ets lists the aliases).
  titlebarIcons?: Record<string, string>;
  // The app's window colour, shown until its first frame.
  background?: string;
}

const RUNTIME_DATA = 'runtime.tar';
// `ohos-wl hap` packs a bottle's hnp programs as the private native package
// wlapp 1.0, which the system installs here (module.json5 hnpPackages).
const HNP_PREFIX = 'hnp:';
// The request file takeRequestFile reads, and how often it looks.
const REQUEST_FILE = 'ohos_wl_request';
const REQUEST_POLL_MS = 250;
const HNP_BIN = '/data/app/wlapp.org/wlapp_1.0/bin';

class NativeSurfaceController extends XComponentController {
  surfaceId: bigint = BigInt(0);
  surfaceWidth: number = 0;
  surfaceHeight: number = 0;
  // Called once the view has its real size, so the app starts at it.
  onSized: () => void = () => {};

  onSurfaceCreated(surfaceId: string): void {
    this.surfaceId = BigInt(surfaceId);
    runtime.createRenderer(this.surfaceId);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    this.surfaceId = BigInt(surfaceId);
    this.surfaceWidth = rect.surfaceWidth;
    this.surfaceHeight = rect.surfaceHeight;
    runtime.resizeRenderer(rect.surfaceWidth, rect.surfaceHeight);
    this.onSized();
  }

  onSurfaceDestroyed(surfaceId: string): void {
    runtime.destroyRenderer();
  }
}

// A FileChooser dialog from the portal (compositor/portal/session_bus.cpp).
interface PortalFilter {
  name: string;
  suffixes: string[];
}

// A user folder an app may open once the user grants it the folder's
// permission. App storage refuses symlinks, so programs reach it by its
// real path, named in the XDG user directories.
interface UserFolder {
  name: string;
  permission: Permissions;
  path: string;
}

interface FolderRequest {
  folder: string;
}

interface PortalDialog {
  title?: string;
  multiple?: boolean;
  directory?: boolean;
  acceptLabel?: string;
  currentName?: string;
  currentFolder?: string;
  filters?: PortalFilter[];
}

// The native title bar's height in vp; the HarmonyOS window buttons centre on it.
const TITLE_BAR_HEIGHT = 56;
// The status bar's height in vp, which a hover title bar sits below.
const PC_STATUS_BAR_HEIGHT = 40;
// The HarmonyOS floating window corner radius (ArkUI CONTAINER_OUTER_RADIUS).
const WINDOW_RADIUS = 16;
// Ctrl+Alt+Shift+D toggles the diagnostics overlay (evdev codes). The default
// keymaps of Zed, Ghostty, and GTK leave it unbound; Zed binds ctrl-alt-shift-f.
const DIAGNOSTICS_KEY = 32;
const CTRL_KEYS = [29, 97];
const ALT_KEYS = [56, 100];
const SHIFT_KEYS = [42, 54];
const META_KEYS = [125, 126];
// The hover title bar comes out from under the status bar. HarmonyOS publishes no timing for the
// status bar, so its layer's state on the device gave about 600 ms for its hide.
const TITLE_BAR_SLIDE: TransitionEffect = TransitionEffect.asymmetric(
  TransitionEffect.move(TransitionEdge.TOP).animation({ duration: 250, curve: Curve.EaseInOut }),
  TransitionEffect.move(TransitionEdge.TOP).animation({ duration: 600, curve: Curve.EaseInOut }));
// The overlay grows out of the corner it sits in on the moderate spring and leaves on a
// quicker tween, as the DP monitor's overlay does.
const OVERLAY_SHOW: TransitionEffect = TransitionEffect.OPACITY
  .combine(TransitionEffect.scale({ x: 0.96, y: 0.96, centerX: '100%', centerY: '100%' }))
  .animation({ curve: SPRING_MODERATE });
const OVERLAY_HIDE: TransitionEffect = TransitionEffect.OPACITY
  .animation({ duration: EXIT_MODERATE_MS, curve: Curve.EaseIn });

@Entry
@Component
struct Index {
  @State status: string = 'Starting compositor';
  @StorageLink('launchSerial') @Watch('onLaunchRequest') launchSerial: number = 0;
  @State toplevelId: number = 0;
  private launchedSerial: number = 0;
  private folders?: UserFolder[];
  private deniedFolders: Set<string> = new Set();
  private pasteboardDenied: boolean = false;
  private requestTimer: number = -1;
  private runningAppId: string = '';
  private appPid: number = 0;
  private pressedKeys: Set<number> = new Set<number>();
  // The diagnostics overlay's latest numbers; undefined while it is off.
  @State diagnostics: Summary | undefined = undefined;
  @State diagnosticsFps: number = -1;
  private diagnosticsSampler: DiagnosticsSampler = new DiagnosticsSampler();
  // The D of a diagnostics chord is down; its release is not forwarded either.
  private diagnosticsKeyHeld: boolean = false;
  // Toplevels of apps the launcher stopped itself; their windows close
  // without closing this HarmonyOS window.
  private stoppedToplevels: Set<number> = new Set<number>();
  private controller: NativeSurfaceController = new NativeSurfaceController();
  private density: number = 1;
  // Keeps a window that fills the screen clear of the display's rounded corners.
  @State corners: DisplayCorners = NO_CORNERS;
  private touchpad: TouchpadScroll | undefined = undefined;
  // Holds the native node that receives HarmonyOS touchpad coasting.
  private coastingSlot: NodeContent = new NodeContent();
  private serverReady: boolean = false;
  private manifest: AppManifest | undefined = undefined;
  @State appBackground: string = '#000000';
  // The app's header bar drawn as a HarmonyOS title bar (NativeTitleBar).
  @State titleBar: TitleBarModel | undefined = undefined;
  @State windowTitle: string = '';
  @State systemButtons: number = 148;
  @State titlebarMode: string | undefined = undefined;
  @State titleBarShown: boolean = false;
  private pointerY: number = 0;
  private pointerRising: boolean = false;
  @State cornerRuler: boolean = false;
  // The app surface's top left in window vp, for drag coordinates.
  private surfaceOrigin: Position = { x: 0, y: 0 };
  private closeRequestedAt: number = 0;
  // Which side draws the title bar, re-applied once the window settles.
  private systemDecorations: boolean = true;
  @State maximized: boolean = false;
  private maximizedAssumed: boolean = false;
  @State fullScreen: boolean = false;

  aboutToAppear(): void {
    this.density = display.getDefaultDisplaySync().densityPixels;
    this.corners = displayCorners(this.density);
    console.info(`CORNER_INSET ${JSON.stringify(this.corners)}`);
    // HarmonyOS sends a touchpad fling's momentum only to components that ask.
    runtime.watchCoasting(this.coastingSlot, (phase: number, deltaX: number, deltaY: number) => {
      this.touchpadScroll().coast(phase, deltaX, deltaY);
    });
    try {
      this.manifest = this.readManifest();
      this.appBackground = this.manifest.background ?? '#000000';
      this.titlebarMode = this.manifest.titlebarMode;
      if (this.manifest.decorations === 'client') this.setSystemDecorations(false);
    } catch (error) {
      console.error(`APP_MANIFEST_ERROR ${JSON.stringify(error)}`);
    }
    runtime.setStateCallback((state: string) => {
      this.status = state;
    });
    runtime.setToplevelCallback((id: number, event: string, json: string) => {
      if (event === 'created') {
        this.toplevelId = id;
        runtime.bindToplevel(id);
        // A window HarmonyOS reopened maximized reports no status change.
        if (this.maximized) runtime.setToplevelMaximized(true);
        // Decor calls made before the window settled can be dropped.
        this.setSystemDecorations(this.systemDecorations);
      } else if (event === 'decoration') {
        // An app that draws its own title bar (GTK header bar, Zed) replaces
        // the HarmonyOS one; its buttons drive this window (applyWindowState).
        const clientSide = (JSON.parse(json) as Record<string, boolean>)['client'] === true;
        this.setSystemDecorations(!clientSide);
      } else if (event === 'destroyed' && this.toplevelId === id) {
        runtime.bindToplevel(0);
        this.toplevelId = 0;
        this.titleBar = undefined;
        this.setSystemDecorations(true);
        if (this.stoppedToplevels.has(id)) {
          this.stoppedToplevels.delete(id);
          return;
        }
        // The app closed its window (its own close button): close this one too.
        this.logWindowRect('close');
        this.stopRunningApp();
        (getContext(this) as common.UIAbilityContext).terminateSelf();
      } else if (event === 'titlebar' && id === this.toplevelId) {
        const model = this.aliasIcons(JSON.parse(json) as TitleBarModel);
        const native = model.native === true;
        this.titleBar = native ? model : undefined;
        this.setSystemDecorations(this.systemDecorations);
      } else if (event === 'title') {
        this.applyWindowTitle(json);
      } else if (id === this.toplevelId) {
        this.applyWindowState(event);
      }
    });
    if (!runtime.startServer(`${this.runtimeDir()}/ohos-wayland-0`)) {
      this.status = 'Failed to start compositor';
      return;
    }
    // The apps' D-Bus session bus: toolkits ask xdg-desktop-portal for file
    // dialogs, and get the HarmonyOS picker.
    if (!runtime.startSessionBus(`${this.runtimeDir()}/bus`,
      (id: number, kind: string, json: string) => this.onPortalRequest(id, kind, json))) {
      console.warn('SESSION_BUS_FAILED');
    }
    runtime.setPasteboardCallback(() => this.onPasteboardRequest());
    // Clients render at the display density; configure sizes are logical.
    runtime.setOutputScale(Math.max(1, Math.round(this.density)));
    // The HarmonyOS close button asks the app to close, so it can save or
    // confirm; the window goes when the app destroys its toplevel. A second
    // click while the app has not answered closes the window anyway.
    try {
      this.mainWindow()?.on('windowEvent', (event: window.WindowEventType) => {
        if (event === window.WindowEventType.WINDOW_INACTIVE) this.releaseKeys();
      });
    } catch (error) {
      console.warn(`WINDOW_EVENT ${JSON.stringify(error)}`);
    }
    try {
      this.mainWindow()?.on('windowWillClose', () => new Promise<boolean>((resolve) => {
        const now = Date.now();
        const pending = now - this.closeRequestedAt < 3000;
        this.closeRequestedAt = now;
        resolve(!pending && this.toplevelId !== 0 && runtime.closeToplevel());
      }));
    } catch (error) {
      console.warn(`WINDOW_WILL_CLOSE ${JSON.stringify(error)}`);
    }
    try {
      this.mainWindow()?.setWindowBackgroundColor('#00000000');
    } catch (error) {
      console.warn(`WINDOW_BACKGROUND ${JSON.stringify(error)}`);
    }
    // Read before the status listener can clear it.
    const reopenFullScreen = fileIo.accessSync(this.fullScreenMarker());
    // CSD apps draw maximized windows without rounded corners or shadows.
    try {
      this.mainWindow()?.on('windowStatusChange', (status: window.WindowStatusType) => {
        this.logWindowRect('status');
        this.rememberFullScreen(status);
        this.fullScreen = status === window.WindowStatusType.FULL_SCREEN;
        // A hidden window reports minimized while a test assumes it is maximized.
        const maximized = this.maximizedAssumed || status === window.WindowStatusType.MAXIMIZE ||
          status === window.WindowStatusType.FULL_SCREEN;
        const changed = maximized !== this.maximized;
        if (this.fullScreen || changed) this.showTitleBar(false);
        this.maximized = maximized;
        this.syncContentInsets();
        // After maximized is set: a window that fills the screen hides the system buttons.
        this.setSystemDecorations(this.systemDecorations);
        if (changed) runtime.setToplevelMaximized(maximized);
      });
    } catch (error) {
      console.error(`WINDOW_STATUS_ERROR ${JSON.stringify(error)}`);
    }
    try {
      // The pointer also leaves when the window loses focus, and that is not the status bar.
      this.mainWindow()?.on('windowHighlightChange', (highlighted: boolean) => {
        if (!highlighted) this.pointerRising = false;
      });
    } catch (error) {
      console.warn(`WINDOW_HIGHLIGHT ${JSON.stringify(error)}`);
    }
    try {
      const status = this.mainWindow()?.getWindowStatus();
      this.fullScreen = status === window.WindowStatusType.FULL_SCREEN;
      this.maximized = status === window.WindowStatusType.MAXIMIZE || this.fullScreen;
      this.syncContentInsets();
      if (reopenFullScreen && !this.fullScreen) {
        this.maximizeWindow()
          ?.catch((error: BusinessError) => console.error(`WINDOW_REOPEN_FULL_SCREEN ${JSON.stringify(error)}`));
      }
    } catch (error) {
      console.warn(`WINDOW_STATUS_INITIAL ${JSON.stringify(error)}`);
    }
    this.logWindowRect('start');
    this.serverReady = true;
    // Start as soon as the view has its size; the timer covers a view that
    // reported it before this point.
    this.controller.onSized = () => this.onLaunchRequest();
    setTimeout(() => this.onLaunchRequest(), 300);
    this.requestTimer = setInterval(() => this.takeRequestFile(), REQUEST_POLL_MS);
  }

  // `ohos_wl.py test` and `screenshot` reach an app they started with
  // `aa pre-start`, which keeps its window hidden, by writing a request file
  // (`test <name>` or `capture`) into the cache. `aa start` would bring the
  // window to the front.
  private takeRequestFile(): void {
    const path = `${this.cacheDir()}/${REQUEST_FILE}`;
    if (!fileIo.accessSync(path)) return;
    const text = fileIo.readTextSync(path);
    if (!text.endsWith('\n')) return;
    fileIo.unlinkSync(path);
    const words = text.trim().split(' ');
    const verb = words[0];
    // `window <x> <y> <width> <height>` places the window, and `close` asks
    // the app to close it, as the close button does; restore tests use both.
    if (verb === 'window' && words.length === 5) {
      const rect = words.slice(1).map((word: string) => Number(word));
      this.mainWindow()?.moveWindowTo(rect[0], rect[1])
        .then(() => this.mainWindow()?.resize(rect[2], rect[3]))
        .then(() => this.logWindowRect('placed'))
        .catch((error: BusinessError) => console.error(`WINDOW_PLACE_ERROR ${JSON.stringify(error)}`));
      return;
    }
    if (verb === 'close') {
      runtime.closeToplevel();
      return;
    }
    // `state fullscreen|maximize|recover` changes the window as its controls do.
    if (verb === 'state' && words.length === 2) {
      const win = this.mainWindow();
      const change = words[1] === 'fullscreen' ? win?.maximize(window.MaximizePresentation.ENTER_IMMERSIVE)
        : words[1] === 'maximize' ? win?.maximize(window.MaximizePresentation.EXIT_IMMERSIVE) : win?.recover();
      change?.catch((error: BusinessError) => console.error(`WINDOW_STATE_ERROR ${JSON.stringify(error)}`));
      return;
    }
    // `replay <file>` plays recorded input into the running app.
    if (verb === 'replay' && words.length > 1) {
      this.replayInput(words[1]);
      return;
    }
    // `corner-ruler` fills the screen with the rounded-corner calibration; `corner-ruler off` ends it.
    if (verb === 'corner-ruler') {
      this.cornerRuler = words[1] !== 'off';
      this.syncContentInsets();
      const win = this.mainWindow();
      (this.cornerRuler ? win?.maximize(window.MaximizePresentation.ENTER_IMMERSIVE) : win?.recover())
        ?.catch((error: BusinessError) => console.error(`CORNER_RULER ${JSON.stringify(error)}`));
      return;
    }
    if (verb === 'overlay-gallery') {
      this.renderOverlayGallery();
      return;
    }
    // window.maximize() would show a hidden window, so `test <name> maximized`
    // assumes the state: the app and its client behave as maximized, the
    // HarmonyOS window does not change.
    if (verb === 'test') {
      const assume = words[2] === 'maximized';
      if (assume || this.maximizedAssumed) this.maximized = assume;
      this.maximizedAssumed = assume;
      this.titlebarMode = this.manifest?.tests?.[words[1]]?.titlebarMode ?? this.manifest?.titlebarMode;
      this.syncContentInsets();
    }
    AppStorage.setOrCreate<string>('launchTest', verb === 'test' && words.length > 1 ? words[1] : '');
    AppStorage.setOrCreate<boolean>('launchCapture', verb === 'capture');
    AppStorage.setOrCreate<number>('launchSerial', (AppStorage.get<number>('launchSerial') ?? 0) + 1);
  }

  aboutToDisappear(): void {
    clearInterval(this.requestTimer);
    this.showTitleBar(false);
    this.diagnosticsSampler.stop();
    this.stopRunningApp();
    runtime.stopGpuHost();
    runtime.destroyRenderer();
    runtime.stopServer();
  }

  onLaunchRequest(): void {
    if (!this.serverReady) return;
    if (this.launchedSerial === this.launchSerial && this.launchSerial !== 0) return;
    this.launchedSerial = this.launchSerial;
    const test = AppStorage.get<string>('launchTest') ?? '';
    if (AppStorage.get<boolean>('launchCapture') ?? false) {
      runtime.captureFrame(`${this.cacheDir()}/ohos_wl_capture.ppm`);
      this.captureWindow(`${this.cacheDir()}/ohos_wl_window.png`);
      if (this.runningAppId.length > 0) return;
    }
    // Tapping the icon of a running app only brings it to the front.
    if (this.runningAppId.length > 0 && test.length === 0) return;
    try {
      this.launch(test);
    } catch (error) {
      const failure = error as BusinessError;
      console.error(`APP_LAUNCH_ERROR code=${failure.code} ${failure.message}`);
      this.status = `Launch failed: ${failure.message}`;
    }
  }

  private launch(test: string): void {
    const manifest = this.manifest ?? this.readManifest();
    for (const archive of [RUNTIME_DATA].concat(manifest.data ?? [])) {
      this.unpackData(archive);
    }
    this.writeGtkStyle();
    this.writeUserDirs();
    for (const file of manifest.files ?? []) {
      const path = this.expand(file.path);
      if (file.ifMissing === true && fileIo.accessSync(path)) {
        this.ensureJsonKeys(path, file.ensureKeys);
        continue;
      }
      this.writeText(path, this.expand(file.text));
    }
    if ((manifest.setup ?? []).includes('ssh-key')) {
      console.info(`OHOS_WL_SSH_PUBLIC_KEY=${runtime.ensureSshKey(`${this.homeDir()}/.ssh`)}`);
    }
    let name = manifest.name;
    let program: AppEntry = manifest;
    const testName = test.length > 0 ? test : (manifest.defaultTest ?? '');
    if (testName.length > 0) {
      const selected = manifest.tests?.[testName];
      if (!selected) throw new Error(`no test ${testName}`);
      name = `${manifest.name}:${testName}`;
      program = selected;
    }
    this.stopRunningApp();
    const gpu = program.gpu !== false;
    // The renderer presents only direct GPU frames while the host runs, so a
    // shared-memory program that follows a GPU one needs it stopped.
    if (gpu) runtime.startGpuHost(this.runtimeDir());
    else runtime.stopGpuHost();
    const environment = this.baseEnvironment()
      .concat(gpu ? runtime.getGpuGuestEnvironment() : [])
      .concat((program.env ?? []).map((value: string) => this.expand(value)));
    // An hnp: program is a stock executable the direct child execs from the
    // HAP's native package; others are libraries it dlopens, or that a plain
    // native child process loads by name from the HAP libs directory.
    const hnp = program.exec.startsWith(HNP_PREFIX);
    const entry = hnp ? 'exec' : (program.entry ?? 'main');
    const path = hnp ? `${HNP_BIN}/${program.exec.substring(HNP_PREFIX.length)}`
      : gpu ? `${this.libsDir()}/${program.exec}` : program.exec;
    const args = (program.args ?? []).map((value: string) => this.expand(value));
    this.runningAppId = `app:${name}`;
    const pid = runtime.launchChild(this.runningAppId, path, entry, args, environment);
    this.appPid = pid;
    console.info(`APP_LAUNCHED name=${name} pid=${pid}`);
    if (program.replay) this.replayInput(this.expand(program.replay));
    this.status = `${name} pid ${pid}`;
  }

  private stopRunningApp(): void {
    if (this.runningAppId.length === 0) return;
    if (this.toplevelId !== 0) this.stoppedToplevels.add(this.toplevelId);
    try {
      runtime.stopChild(this.runningAppId);
    } catch (error) {
      console.warn(`APP_STOP_ERROR ${JSON.stringify(error)}`);
    }
    this.runningAppId = '';
    this.appPid = 0;
  }

  private readManifest(): AppManifest {
    const raw = getContext(this).resourceManager.getRawFileContentSync('app.json');
    return JSON.parse(buffer.from(raw.buffer).toString('utf-8')) as AppManifest;
  }

  // Unpacks a rawfile tar into ${FILES} once per installed HAP version.
  private unpackData(archive: string): void {
    const filesDir = getContext(this).filesDir;
    const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
    const stampPath = `${filesDir}/.unpacked-${archive}`;
    const stamp = `${info.versionCode}:${info.updateTime}`;
    if (fileIo.accessSync(stampPath) && fileIo.readTextSync(stampPath) === stamp) return;
    const descriptor = getContext(this).resourceManager.getRawFdSync(archive);
    try {
      const entries = runtime.extractTar(descriptor.fd, descriptor.offset, descriptor.length, filesDir);
      if (entries < 0) throw new Error(`extract ${archive} failed: ${entries}`);
      console.info(`APP_DATA ${archive} entries=${entries}`);
    } finally {
      getContext(this).resourceManager.closeRawFdSync(archive);
    }
    this.writeText(stampPath, stamp);
  }

  private darkMode(): boolean {
    return (getContext(this) as common.UIAbilityContext).config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
  }

  // GTK reads user CSS only from $XDG_CONFIG_HOME, so the runtime's
  // HarmonyOS stylesheet for the current colour mode is copied there; the user settings.ini adds the
  // HarmonyOS light or dark mode on top of the runtime's settings.ini.
  private writeGtkStyle(): void {
    const files = getContext(this).filesDir;
    const gtkConfig = `${this.homeDir()}/.config/gtk-4.0`;
    if (!fileIo.accessSync(gtkConfig)) fileIo.mkdirSync(gtkConfig, true);
    const mode = this.darkMode() ? 'dark' : 'light';
    const style = fileIo.readTextSync(`${files}/runtime/etc/gtk-4.0/harmonyos-${mode}.css`);
    const vpToPx = this.density / Math.max(1, Math.round(this.density));
    this.writeText(`${gtkConfig}/gtk.css`, style + titleBarCss(this.darkMode(), vpToPx));
    // The system settings file, so an app's own ~/.config/gtk-4.0/settings.ini
    // keeps what the user set and overrides the color scheme only if it says so.
    const userSettings = `${gtkConfig}/settings.ini`;
    if (fileIo.accessSync(userSettings) &&
      /^\[Settings\]\ngtk-interface-color-scheme=(dark|light)\n$/.test(fileIo.readTextSync(userSettings))) {
      fileIo.unlinkSync(userSettings);
    }
    const systemGtk = `${files}/runtime/etc/gtk-4.0`;
    if (!fileIo.accessSync(systemGtk)) fileIo.mkdirSync(systemGtk, true);
    // GTK, and apps that read gtk-xft-dpi such as Ghostty, size text at that
    // DPI times the client scale. The scale is the density rounded to an
    // integer (2 for 1.8), so the DPI takes the rounding back out and adds
    // the HarmonyOS font size setting.
    const fontSizeScale = (getContext(this) as common.UIAbilityContext).config.fontSizeScale ?? 1;
    const xftDpi = Math.round(96 * 1024 * this.density / Math.max(1, Math.round(this.density)) * fontSizeScale);
    console.info(`TEXT_SCALE density=${this.density} fontSizeScale=${fontSizeScale} gtk-xft-dpi=${xftDpi}`);
    this.writeText(`${systemGtk}/settings.ini`,
      `[Settings]\ngtk-interface-color-scheme=${this.darkMode() ? 'dark' : 'light'}\ngtk-xft-dpi=${xftDpi}\n`);
  }

  private baseEnvironment(): string[] {
    const files = getContext(this).filesDir;
    const share = `${files}/runtime/share`;
    return [
      `XDG_RUNTIME_DIR=${this.runtimeDir()}`,
      'WAYLAND_DISPLAY=ohos-wayland-0',
      `DBUS_SESSION_BUS_ADDRESS=unix:path=${this.runtimeDir()}/bus`,
      `HOME=${this.homeDir()}`,
      `OHOS_USER_FOLDERS=${this.userFolders().map((folder: UserFolder) => `${folder.name}=${folder.path}`).join(';')}`,
      `XDG_DATA_DIRS=${files}/share:${share}`,
      `XDG_CONFIG_HOME=${this.homeDir()}/.config`,
      `XDG_CONFIG_DIRS=${files}/runtime/etc`,
      // Settings go to a file in HOME; GLib's memory backend would lose them at exit.
      'GSETTINGS_BACKEND=keyfile',
      // Mesa keeps compiled GL programs and Zink pipelines across launches.
      `MESA_SHADER_CACHE_DIR=${this.cacheDir()}/mesa_shader_cache`,
      `ADW_DEBUG_COLOR_SCHEME=${this.darkMode() ? 'prefer-dark' : 'prefer-light'}`,
      `FONTCONFIG_FILE=${files}/runtime/fonts.conf`,
      `XKB_CONFIG_ROOT=${share}/X11/xkb`,
      `XLOCALEDIR=${share}/X11/locale`,
      `XCURSOR_PATH=${share}/icons`,
      'XCURSOR_THEME=Adwaita',
      'GDK_BACKEND=wayland',
      'GSK_RENDERER=gl',
      'GTK_A11Y=none',
    ];
  }

  private expand(value: string): string {
    return value
      .replace(/\$\{LIBS\}/g, this.libsDir())
      .replace(/\$\{HNP\}/g, HNP_BIN)
      .replace(/\$\{FILES\}/g, getContext(this).filesDir)
      .replace(/\$\{CACHE\}/g, this.cacheDir())
      .replace(/\$\{HOME\}/g, this.homeDir())
      .replace(/\$\{RUNTIME_DIR\}/g, this.runtimeDir());
  }

  private ensureJsonKeys(path: string, keys?: Record<string, string>): void {
    if (!keys) return;
    let text = fileIo.readTextSync(path);
    const brace = text.indexOf('{');
    if (brace < 0) return;
    let added = '';
    for (const key of Object.keys(keys)) {
      if (!text.includes(`"${key}"`)) added += `\n  "${key}": ${keys[key]},`;
    }
    if (added.length === 0) return;
    text = text.substring(0, brace + 1) + added + text.substring(brace + 1);
    this.writeText(path, text);
  }

  private writeText(path: string, text: string): void {
    const directory = path.substring(0, path.lastIndexOf('/'));
    if (!fileIo.accessSync(directory)) fileIo.mkdirSync(directory, true);
    const file = fileIo.openSync(path,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    fileIo.writeSync(file.fd, text);
    fileIo.closeSync(file);
  }

  // The application cache (/data/storage/el2/base/cache): the SSH key, app
  // output logs, and captures live here, not in the module's cache.
  private cacheDir(): string {
    return getContext(this).getApplicationContext().cacheDir;
  }

  private libsDir(): string {
    return `${getContext(this).bundleCodeDir}/libs/arm64`;
  }

  private homeDir(): string {
    const home = `${getContext(this).filesDir}/home`;
    if (!fileIo.accessSync(home)) fileIo.mkdirSync(home, true);
    return home;
  }

  private runtimeDir(): string {
    const dir = `${getContext(this).filesDir}/wayland`;
    if (!fileIo.accessSync(dir)) fileIo.mkdirSync(dir, true);
    return dir;
  }

  // This window only, with its ArkUI title bar (window.snapshot).
  private captureWindow(path: string): void {
    this.mainWindow()?.snapshot().then((pixels: image.PixelMap) => this.writePng(pixels, path))
      .catch((error: BusinessError) => console.warn(`WINDOW_CAPTURE ${JSON.stringify(error)}`));
  }

  private writePng(pixels: image.PixelMap, path: string): Promise<void> {
    const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    return image.createImagePacker().packToFile(pixels, file.fd, { format: 'image/png', quality: 100 })
      .finally(() => {
        fileIo.closeSync(file);
        pixels.release();
      });
  }

  // `overlay-gallery` renders the diagnostics overlay in each named state, dark
  // and light, offscreen to cache/ohos_wl_overlay_<state>-<mode>.png. A hidden
  // window cannot be snapshotted, and this needs no window in front.
  private renderOverlayGallery(): void {
    const snapshot = this.getUIContext().getComponentSnapshot();
    let chain: Promise<void> = Promise.resolve();
    for (const state of GALLERY_STATES) {
      for (const mode of [ThemeColorMode.DARK, ThemeColorMode.LIGHT]) {
        const name = `${state.name}-${mode === ThemeColorMode.DARK ? 'dark' : 'light'}`;
        chain = chain.then(() => snapshot.createFromBuilder(() => {
          this.galleryItem(state, mode)
        }, 320, true)).then((pixels: image.PixelMap) =>
          this.writePng(pixels, `${this.cacheDir()}/ohos_wl_overlay_${name}.png`))
          .catch((error: BusinessError) => console.warn(`OVERLAY_GALLERY ${name} ${JSON.stringify(error)}`));
      }
    }
    chain.then(() => console.info('OVERLAY_GALLERY done'));
  }

  @Builder
  titleBarView() {
    NativeTitleBar({
      model: this.titleBar ?? { serial: 0, native: true, title: this.windowTitle },
      barHeight: TITLE_BAR_HEIGHT,
      frosted: this.hoversTitleBar(),
      systemButtons: this.systemButtons,
      ownWindowButtons: this.maximized,
      placements: this.manifest?.titlebar ?? {},
      activate: (id: number, action: string, target: string) => {
        runtime.activateTitlebar(`${this.titleBar?.serial ?? 0}\t${id}\t${action}\t${target}`);
      },
      startMoving: () => {
        this.mainWindow()?.startMoving().catch((error: BusinessError) => {
          console.warn(`START_MOVING ${JSON.stringify(error)}`);
        });
      },
      minimize: () => {
        this.mainWindow()?.minimize().catch((error: BusinessError) => {
          console.warn(`MINIMIZE ${JSON.stringify(error)}`);
        });
      },
      close: () => {
        (getContext(this) as common.UIAbilityContext).terminateSelf();
      },
      toggleMaximized: () => {
        const win = this.mainWindow();
        if (!win) return;
        (this.maximized ? win.recover() : this.maximizeWindow())?.catch((error: BusinessError) => {
          console.warn(`TOGGLE_MAXIMIZED ${JSON.stringify(error)}`);
        });
      },
    })
  }

  @Builder
  galleryItem(state: GalleryState, mode: ThemeColorMode) {
    DiagnosticsGallery(state, mode)
  }

  // HarmonyOS reopens a window closed in fullscreen at its floating rect from
  // before; like macOS, the runtime puts it back in fullscreen. The marker
  // follows every change, so a force-stop leaves it right too. Minimizing
  // keeps it: the window returns to the state it was minimized from.
  private fullScreenMarker(): string {
    return `${getContext(this).filesDir}/window_fullscreen`;
  }

  private rememberFullScreen(status: window.WindowStatusType): void {
    if (status === window.WindowStatusType.MINIMIZE) return;
    const marker = this.fullScreenMarker();
    try {
      if (status === window.WindowStatusType.FULL_SCREEN) {
        fileIo.closeSync(fileIo.openSync(marker, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY));
      } else if (fileIo.accessSync(marker)) {
        fileIo.unlinkSync(marker);
      }
    } catch (error) {
      console.warn(`WINDOW_FULL_SCREEN_MARKER ${JSON.stringify(error)}`);
    }
  }

  private logWindowRect(when: string): void {
    try {
      const properties = this.mainWindow()?.getWindowProperties();
      const rect = properties?.windowRect;
      console.info(`WINDOW_RECT ${when} ${rect?.left},${rect?.top} ${rect?.width}x${rect?.height} ` +
        `status=${this.mainWindow()?.getWindowStatus()} immersive=${properties?.isLayoutFullScreen} ` +
        `full=${properties?.isFullScreen} drawable=${JSON.stringify(properties?.drawableRect)}`);
    } catch (error) {
      console.warn(`WINDOW_RECT ${when} ${JSON.stringify(error)}`);
    }
  }

  private mainWindow(): window.Window | undefined {
    return AppStorage.get<window.Window>('mainWindow');
  }

  // An xdg-desktop-portal FileChooser request, shown as the HarmonyOS
  // document picker. The picker grants the app access to what it returns.
  private onPortalRequest(id: number, kind: string, json: string): void {
    if (kind === 'folder') {
      this.onFolderRequest(id, (JSON.parse(json) as FolderRequest).folder);
      return;
    }
    const dialog = JSON.parse(json) as PortalDialog;
    const documents = new picker.DocumentViewPicker(getContext(this) as common.UIAbilityContext);
    const filters = (dialog.filters ?? []).map((filter: PortalFilter) => `${filter.name}|${filter.suffixes.join(',')}`);
    // A dialog opens where the app asks, when that is a user folder, and in
    // Documents otherwise.
    const documentsFolder = this.userFolders().find((candidate: UserFolder) => candidate.name === 'documents');
    const start = dialog.currentFolder?.startsWith('/storage/Users/') ? dialog.currentFolder : documentsFolder?.path;
    const folder = start ? `file://docs${start}` : '';
    const answer = (uris: string[]) => {
      runtime.portalRespond(id, uris.length > 0 ? 0 : 1, uris.map((uri: string) => this.portalUri(uri)));
    };
    const fail = (error: BusinessError) => {
      console.error(`PORTAL_PICKER_ERROR ${JSON.stringify(error)}`);
      runtime.portalRespond(id, 2, []);
    };
    try {
      if (kind === 'save') {
        const options = new picker.DocumentSaveOptions();
        if (dialog.currentName) options.newFileNames = [dialog.currentName];
        if (filters.length > 0) options.fileSuffixChoices = filters;
        if (folder) options.defaultFilePathUri = folder;
        documents.save(options).then(answer).catch(fail);
      } else {
        const options = new picker.DocumentSelectOptions();
        options.maxSelectNumber = dialog.multiple ? 500 : 1;
        options.selectMode = dialog.directory ? picker.DocumentSelectMode.FOLDER : picker.DocumentSelectMode.FILE;
        if (filters.length > 0 && !dialog.directory) options.fileSuffixFilters = filters;
        if (folder) options.defaultFilePathUri = folder;
        documents.select(options).then(answer).catch(fail);
      }
    } catch (error) {
      fail(error as BusinessError);
    }
  }

  // The HarmonyOS dialog asks the user. Once a permission is denied, HarmonyOS
  // shows that dialog no more, so the settings dialog asks instead. Callers
  // ask at most once per app run.
  private askPermission(permission: Permissions, answer: (granted: boolean) => void,
    fail: (error: BusinessError) => void): void {
    const context = getContext(this) as common.UIAbilityContext;
    const manager = abilityAccessCtrl.createAtManager();
    manager.requestPermissionsFromUser(context, [permission]).then((result) => {
      if (result.authResults[0] === 0 || result.dialogShownResults?.[0] !== false) {
        answer(result.authResults[0] === 0);
        return;
      }
      manager.requestPermissionOnSetting(context, [permission]).then((statuses) => {
        answer(statuses[0] === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED);
      }).catch(fail);
    }).catch(fail);
  }

  // A program was refused one of the user's folders (exec_preload.cpp).
  private onFolderRequest(id: number, name: string): void {
    const folder = this.userFolders().find((candidate: UserFolder) => candidate.name === name);
    if (!folder || this.deniedFolders.has(name)) {
      runtime.portalRespond(id, folder ? 1 : 2, []);
      return;
    }
    this.askPermission(folder.permission, (granted: boolean) => {
      console.info(`FOLDER_REQUEST ${name} granted=${granted}`);
      if (!granted) this.deniedFolders.add(name);
      runtime.portalRespond(id, granted ? 0 : 1, []);
    }, (error: BusinessError) => {
      console.error(`FOLDER_REQUEST_ERROR ${name} ${JSON.stringify(error)}`);
      runtime.portalRespond(id, 2, []);
    });
  }

  // An app pasted what another app copied (compositor/data_device.cpp).
  private onPasteboardRequest(): void {
    if (this.pasteboardDenied) {
      runtime.pasteboardPermission(false);
      return;
    }
    this.askPermission('ohos.permission.READ_PASTEBOARD', (granted: boolean) => {
      console.info(`PASTEBOARD_REQUEST granted=${granted}`);
      this.pasteboardDenied = !granted;
      runtime.pasteboardPermission(granted);
    }, (error: BusinessError) => {
      console.error(`PASTEBOARD_REQUEST_ERROR ${JSON.stringify(error)}`);
      runtime.pasteboardPermission(false);
    });
  }

  private userFolders(): UserFolder[] {
    if (this.folders !== undefined) return this.folders;
    const folders: UserFolder[] = [];
    const add = (name: string, permission: Permissions, path: () => string) => {
      try {
        folders.push({ name: name, permission: permission, path: path() });
      } catch (error) {
        console.warn(`USER_FOLDER_MISSING ${name} ${JSON.stringify(error)}`);
      }
    };
    add('documents', 'ohos.permission.READ_WRITE_DOCUMENTS_DIRECTORY', () => Environment.getUserDocumentDir());
    add('desktop', 'ohos.permission.READ_WRITE_DESKTOP_DIRECTORY', () => Environment.getUserDesktopDir());
    add('download', 'ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY', () => Environment.getUserDownloadDir());
    this.folders = folders;
    return folders;
  }

  // The user's folders as XDG user directories, where file dialogs and
  // other Linux programs look for them.
  private writeUserDirs(): void {
    const names = this.userFolders().map((folder: UserFolder) => `XDG_${folder.name.toUpperCase()}_DIR="${folder.path}"`);
    this.writeText(`${this.homeDir()}/.config/user-dirs.dirs`, names.join('\n') + '\n');
  }

  // A picker URI (file://docs/storage/Users/...) as the file:// URI of the
  // path the app's processes open.
  private portalUri(uri: string): string {
    return `file://${encodeURI(new fileUri.FileUri(uri).path)}`;
  }

  // A window that fills the screen, while it is not measuring the corners themselves.
  private fillsScreen(): boolean {
    return this.maximized && !this.cornerRuler;
  }

  // Fullscreen keeps the system title on its hover path, and an app with its own bar needs no other.
  private showsTitleBar(): boolean {
    if (this.maximized) return !this.fullScreen && !this.manifest?.topBar && !this.hoversTitleBar();
    return this.titleBar !== undefined;
  }

  private hoversTitleBar(): boolean {
    return this.titlebarMode === 'hover' && this.maximized && !this.fullScreen && !this.manifest?.topBar;
  }

  private contentInsets(): CornerInsets {
    if (!this.fillsScreen()) return NO_INSETS;
    const insets = this.corners.insets;
    // The rounded corners only crop the background of a bar along the top.
    if (this.manifest?.topBar || this.showsTitleBar()) {
      return { top: 0, right: insets.right, bottom: insets.bottom, left: insets.left };
    }
    return insets;
  }

  // The app area's top corners are square under a title bar.
  private appCorners(): BorderRadiuses {
    const radius = this.fillsScreen() ? this.corners.drawnRadius : 0;
    const top = this.showsTitleBar() ? 0 : radius;
    return { topLeft: top, topRight: top, bottomLeft: radius, bottomRight: radius };
  }

  // The renderer keeps the app inside these margins and continues its edge pixels across them,
  // so the margins look like the app whatever it draws.
  private syncContentInsets(): void {
    const insets = this.contentInsets();
    const margins = [insets.left, insets.top, insets.right, insets.bottom]
      .map((value: number) => Math.round(this.px(value)));
    runtime.setContentInsets(margins[0], margins[1], margins[2], margins[3]);
    console.info(`CONTENT_INSETS ${margins.join(' ')} bar=${this.showsTitleBar()}`);
  }

  // The native title bar shows only in true maximized mode, because fullscreen keeps the system
  // title. An app with its own bar needs neither, so it goes fullscreen.
  private maximizeWindow(): Promise<void> | undefined {
    return this.mainWindow()?.maximize(this.manifest?.topBar ? window.MaximizePresentation.ENTER_IMMERSIVE
      : window.MaximizePresentation.EXIT_IMMERSIVE);
  }

  // The hover title bar shows with the status bar and hides once the pointer is in the app below it.
  // The status bar takes the pointer as it shows, so the window sees the pointer leave. A fast
  // pointer leaves from wherever it last reported, so that can be far below the edge, but it is
  // still rising through the top half. Leaving at the bottom is the dock.
  private followTitleBarHover(windowY: number, action: MouseAction): void {
    if (!this.hoversTitleBar()) return;
    if (action === MouseAction.LEAVE_WINDOW) {
      const height = (this.mainWindow()?.getWindowProperties().windowRect.height ?? 0) / this.density;
      if (this.pointerRising && windowY < height / 2) this.showTitleBar(true);
      return;
    }
    if (windowY !== this.pointerY) {
      this.pointerRising = windowY < this.pointerY;
      this.pointerY = windowY;
    }
    if (windowY > PC_STATUS_BAR_HEIGHT + TITLE_BAR_HEIGHT) this.showTitleBar(false);
  }

  private showTitleBar(shown: boolean): void {
    if (shown === this.titleBarShown) return;
    this.titleBarShown = shown;
    console.info(`TITLEBAR_HOVER shown=${shown}`);
  }

  // A HarmonyOS drag over the app: 0 enter, 1 move, 2 leave, 3 drop.
  private forwardDrag(kind: number, event: DragEvent): void {
    if (this.toplevelId === 0) return;
    const x = this.px(event.getWindowX() - Number(this.surfaceOrigin.x ?? 0));
    const y = this.px(event.getWindowY() - Number(this.surfaceOrigin.y ?? 0));
    if (kind === 0) {
      runtime.dragEvent(0, x, y, dragMimeTypes(event.getSummary()), []);
    } else if (kind === 3) {
      try {
        const drop = dropPayload(event.getData(), (uri: string) => this.portalUri(uri));
        runtime.dragEvent(3, x, y, drop.mimeTypes, drop.payloads);
        event.setResult(DragResult.DRAG_SUCCESSFUL);
      } catch (error) {
        console.error(`DROP_ERROR ${JSON.stringify(error)}`);
        runtime.dragEvent(2, x, y, [], []);
        event.setResult(DragResult.DRAG_FAILED);
      }
    } else {
      runtime.dragEvent(kind, x, y, [], []);
    }
  }

  private aliasIcons(model: TitleBarModel): TitleBarModel {
    const aliases = this.manifest?.titlebarIcons ?? {};
    const alias = (items?: TitleBarItem[]) => {
      for (const item of items ?? []) {
        const name = iconName(item);
        if (aliases[name]) item.icon = aliases[name];
        alias(item.options);
      }
    };
    alias(model.start);
    alias(model.center);
    alias(model.end);
    return model;
  }

  private setSystemDecorations(visible: boolean): void {
    this.systemDecorations = visible;
    // A native title bar replaces the HarmonyOS title but keeps its window
    // buttons, the way HarmonyOS apps with custom title bars look.
    const native = this.titleBar !== undefined;
    try {
      this.mainWindow()?.setWindowDecorVisible(visible && !native && !this.maximized);
    } catch (error) {
      console.error(`DECOR_ERROR ${JSON.stringify(error)}`);
    }
    // The title bar draws window buttons in maximize mode. Fullscreen keeps
    // the system title, status bar, and dock on their shared hover path.
    const buttons = (visible || native) && (!this.maximized || this.fullScreen);
    try {
      this.mainWindow()?.setWindowTitleButtonVisible(buttons, buttons, buttons);
    } catch (error) {
      console.warn(`TITLE_BUTTONS ${JSON.stringify(error)}`);
    }
    this.mainWindow()?.setTitleAndDockHoverShown(true, true)
      .catch((error: BusinessError) => console.warn(`TITLE_HOVER ${JSON.stringify(error)}`));
    if (!native) return;
    try {
      this.mainWindow()?.setWindowDecorHeight(TITLE_BAR_HEIGHT);
      const rect = this.mainWindow()?.getTitleButtonRect();
      if (rect && rect.width > 0) {
        // The rectangle includes the buttons' margins; add a small gap.
        this.systemButtons = rect.width + 8;
        console.info(`TITLE_BUTTON_RECT ${JSON.stringify(rect)}`);
      }
    } catch (error) {
      console.warn(`TITLE_BUTTON_RECT ${JSON.stringify(error)}`);
    }
  }

  private applyWindowTitle(json: string): void {
    try {
      const title = (JSON.parse(json) as Record<string, string>)['title'] ?? '';
      if (title.length > 0) {
        this.windowTitle = title;
        this.mainWindow()?.setWindowTitle(title);
      }
    } catch (error) {
      console.error(`TITLE_ERROR ${JSON.stringify(error)}`);
    }
  }

  // xdg_toplevel requests from the client's own title bar drive the
  // HarmonyOS window: drag to move, and the window buttons.
  private applyWindowState(event: string): void {
    const win = this.mainWindow();
    if (!win) return;
    try {
      if (event === 'move_start') {
        win.startMoving();
      } else if (event === 'maximized') {
        this.maximizeWindow();
      } else if (event === 'unmaximized') {
        win.recover();
      } else if (event === 'minimized') {
        win.minimize();
      }
    } catch (error) {
      console.error(`WINDOW_STATE_ERROR ${event} ${JSON.stringify(error)}`);
    }
  }

  // Plays recorded input (fixtures/*.tsv, one event per line, the first
  // field ms since the previous event) at the window centre, through the
  // same calls real input takes. Waits for the window. A scroll line is
  // `ms axis value step source stop`, a key line `ms key evdev down`, and
  // `ms drop text` drops text there, and `ms capture label` writes the next composed frame to
  // cache/ohos_wl_replay_<label>.ppm.
  private replayInput(path: string, waitedMs: number = 0): void {
    if (this.toplevelId === 0) {
      if (waitedMs < 10000) setTimeout(() => this.replayInput(path, waitedMs + 200), 200);
      return;
    }
    const events = fileIo.readTextSync(path).split('\n')
      .filter((line: string) => line.length > 0 && !line.startsWith('#'))
      .map((line: string) => line.split('\t'));
    const x = this.controller.surfaceWidth / 2;
    const y = this.controller.surfaceHeight / 2;
    const cache = this.cacheDir();
    console.info(`INPUT_REPLAY start events=${events.length} ${path}`);
    // A real scroll follows the pointer moving there. Without it the client
    // keeps the enter position, the window corner, and scrolls the widget there.
    runtime.sendPointerEvent(3, x, y, 0, 0, 0, true);
    let at = 1000;
    for (const event of events) {
      at += Number(event[0]);
      if (event[1] === 'coast') {
        setTimeout(() => this.touchpadScroll().coast(Number(event[2]), Number(event[3]), Number(event[4])), at);
      } else if (event[1] === 'drop') {
        setTimeout(() => this.replayDrop(event.slice(2).join('\t'), x, y), at);
      } else if (event[1] === 'key') {
        setTimeout(() => this.forwardKey(Number(event[2]), event[3] === '1'), at);
      } else if (event[1] === 'click') {
        setTimeout(() => {
          runtime.sendPointerEvent(3, Number(event[2]), Number(event[3]), 0, 0, 0, true);
          runtime.sendPointerEvent(1, Number(event[2]), Number(event[3]), mapMouseButton(1), 0, 0, true);
          runtime.sendPointerEvent(2, Number(event[2]), Number(event[3]), mapMouseButton(1), 0, 0, true);
        }, at);
      } else if (event[1] === 'hover') {
        setTimeout(() => this.followTitleBarHover(Number(event[2]),
          event[3] === 'leave' ? MouseAction.LEAVE_WINDOW : MouseAction.Move), at);
      } else if (event[1] === 'capture') {
        setTimeout(() => runtime.captureFrame(`${cache}/ohos_wl_replay_${event[2]}.ppm`), at);
      } else {
        const n = event.map((field: string) => Number(field));
        setTimeout(() => this.replayScroll(n, x, y), at);
      }
    }
    setTimeout(() => console.info('INPUT_REPLAY done'), at + 100);
  }

  // `ms drop text`: a text drag enters, moves, and drops at the window
  // centre, through the call ArkUI drag events take.
  private replayDrop(text: string, x: number, y: number): void {
    const types = ['text/plain;charset=utf-8', 'text/plain', 'UTF8_STRING'];
    runtime.dragEvent(0, x, y, types, []);
    setTimeout(() => runtime.dragEvent(1, x + 10, y + 10, [], []), 100);
    setTimeout(() => runtime.dragEvent(3, x + 10, y + 10, types, types.map(() => text)), 300);
  }

  // A recorded scroll line: touchpad lines take the live touchpad path.
  private replayScroll(n: number[], x: number, y: number): void {
    const axis = n[1];
    const value = n[2];
    const step = n[3];
    const source = n[4];
    const stop = n[5] === 1;
    if (source !== 1) {
      runtime.sendScrollEvent(axis, value, step, x, y, source, stop);
    } else if (stop) {
      if (axis === 0) this.touchpadScroll().lift();
    } else {
      this.touchpadScroll().move(axis === 1 ? value : 0, axis === 0 ? value : 0, step, x, y);
    }
  }

  // Every key for the app, real or replayed, comes through here.
  private forwardKey(key: number, down: boolean): void {
    // Wayland clients repeat keys themselves (wl_keyboard.repeat_info),
    // so ArkUI's auto-repeated Down events are not forwarded.
    if (down === this.pressedKeys.has(key)) return;
    if (down) {
      this.pressedKeys.add(key);
    } else {
      this.pressedKeys.delete(key);
    }
    if (key === DIAGNOSTICS_KEY && (down ? this.diagnosticsChordHeld() : this.diagnosticsKeyHeld)) {
      this.diagnosticsKeyHeld = down;
      if (down) this.toggleDiagnostics();
      return;
    }
    runtime.sendKeyEvent(key, down);
  }

  // A window that loses focus gets no release for the keys still held, so
  // the app would keep a modifier down and read later keys as shortcuts.
  private releaseKeys(): void {
    for (const key of Array.from(this.pressedKeys)) this.forwardKey(key, false);
  }

  private toggleDiagnostics(): void {
    if (this.diagnostics) {
      this.diagnosticsSampler.stop();
      this.diagnostics = undefined;
      this.diagnosticsFps = -1;
      return;
    }
    this.diagnosticsSampler.start(this.appPid, (summary: Summary) => this.diagnostics = summary,
      (fps: number) => this.diagnosticsFps = fps);
  }

  private diagnosticsChordHeld(): boolean {
    const held = (keys: number[]) => keys.some((key: number) => this.pressedKeys.has(key));
    return held(CTRL_KEYS) && held(ALT_KEYS) && held(SHIFT_KEYS) && !held(META_KEYS);
  }

  private touchpadScroll(): TouchpadScroll {
    if (!this.touchpad) {
      this.touchpad = new TouchpadScroll(this.cacheDir());
      console.info(`SCROLL_SCALE density=${this.density}`);
    }
    // A touchpad reports finger travel in vp, which Wayland wants as logical pixels.
    this.touchpad.scale = this.density / Math.max(1, Math.round(this.density));
    return this.touchpad;
  }

  private px(value: number): number {
    return value * this.density;
  }

  build() {
    Column() {
      if (this.showsTitleBar()) {
        this.titleBarView()
      }
      Stack({ alignContent: Alignment.TopStart }) {
        // Only the app is inset, inside this surface (syncContentInsets): the title bar, the ruler,
        // and the overlay keep their places against the window, and the ruler measures the
        // corners themselves, so it runs without an inset.
        Column() {
          XComponent({
            type: XComponentType.SURFACE,
            controller: this.controller,
          })
            .width('100%')
            .height('100%')
            .backgroundColor(this.appBackground)
            .focusable(true)
            .defaultFocus(true)
            .onAreaChange((_old: Area, area: Area) => this.surfaceOrigin = area.globalPosition)
            .allowDrop(DROP_TYPES)
            .onDragEnter((event: DragEvent) => this.forwardDrag(0, event))
            .onDragMove((event: DragEvent) => this.forwardDrag(1, event))
            .onDragLeave((event: DragEvent) => this.forwardDrag(2, event))
            .onDrop((event: DragEvent) => this.forwardDrag(3, event))
            .onMouse((event: MouseEvent) => {
              if (this.toplevelId === 0) return;
              this.followTitleBarHover(event.windowY, event.action);
              runtime.sendPointerEvent(event.action, this.px(event.x), this.px(event.y),
                mapMouseButton(event.button), event.rawDeltaX ?? 0, event.rawDeltaY ?? 0, true);
            })
            .onTouch((event: TouchEvent) => {
              // Mouse-generated touch events use onMouse, so one physical action reaches Wayland once.
              if (this.toplevelId === 0 || event.source !== SourceType.TouchScreen || event.touches.length === 0) return;
              const touch = event.changedTouches[0] ?? event.touches[0];
              const x = this.px(touch.x);
              const y = this.px(touch.y);
              if (event.type === TouchType.Down) {
                runtime.sendPointerEvent(3, x, y, 0, 0, 0, false);
                runtime.sendPointerEvent(1, x, y, mapMouseButton(1), 0, 0, false);
              } else if (event.type === TouchType.Move) {
                runtime.sendPointerEvent(3, x, y, mapMouseButton(1), 0, 0, false);
              } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
                runtime.sendPointerEvent(2, x, y, mapMouseButton(1), 0, 0, false);
              }
            })
            .onAxisEvent((event?: AxisEvent) => {
              if (this.toplevelId === 0 || !event) return;
              const x = this.px(event.x);
              const y = this.px(event.y);
              const step = event.scrollStep ?? 0;
              const ended = event.action === AxisAction.END || event.action === AxisAction.CANCEL;
              if (event.sourceTool === SourceTool.TOUCHPAD) {
                if (ended) {
                  this.touchpadScroll().lift();
                } else {
                  this.touchpadScroll().move(event.getHorizontalAxisValue(), event.getVerticalAxisValue(), step, x, y);
                }
                return;
              }
              // A wheel reports notches, which the compositor turns into wheel steps.
              if (ended) {
                runtime.sendScrollEvent(0, 0, step, x, y, 0, true);
                runtime.sendScrollEvent(1, 0, step, x, y, 0, true);
                return;
              }
              const vertical = event.getVerticalAxisValue();
              const horizontal = event.getHorizontalAxisValue();
              if (vertical !== 0) runtime.sendScrollEvent(0, vertical, step, x, y, 0, false);
              if (horizontal !== 0) runtime.sendScrollEvent(1, horizontal, step, x, y, 0, false);
            })
            .onKeyEvent((event: KeyEvent): boolean => {
              if (this.toplevelId === 0) return false;
              event.stopPropagation();
              const key = OH2EVDEV[event.keyCode] ?? 0;
              if (key === 0) return false;
              this.forwardKey(key, event.type === KeyType.Down);
              // Consumed, so ArkUI does not also move focus on Tab or the arrows.
              return true;
            })
        }
        .width('100%')
        .height('100%')
        // Past the rounded corners, like the display's own bezel.
        .backgroundColor(this.fillsScreen() ? Color.Black : this.appBackground)
        .borderRadius(this.appCorners())
        .clip(true)
        .onMouse((event: MouseEvent) => this.followTitleBarHover(event.windowY, event.action))
        ContentSlot(this.coastingSlot)

        if (this.hoversTitleBar() && this.titleBarShown) {
          Column() {
            this.titleBarView()
          }
          .width('100%')
          // The backdrop runs behind the status bar, which drops away when the pointer moves onto
          // this bar. The bar stays under the pointer and nothing opens up where the status bar was.
          .padding({ top: PC_STATUS_BAR_HEIGHT })
          .backgroundEffect({
            radius: 48,
            saturation: 1.2,
            color: '#B8000000',
            policy: BlurStyleActivePolicy.ALWAYS_ACTIVE,
          })
          .transition(TITLE_BAR_SLIDE)
        }

        if (this.cornerRuler) {
          CornerRuler()
        }

        if (this.diagnostics) {
          DiagnosticsOverlay({ summary: this.diagnostics, fps: this.diagnosticsFps })
            .position({ right: 15, bottom: 15 })
            .transition(TransitionEffect.asymmetric(OVERLAY_SHOW, OVERLAY_HIDE))
        }

        // Startup shows only the app's background; text appears on failure.
        if (this.toplevelId === 0 && this.status.includes('ailed')) {
        Column() {
          Text(this.status)
            .fontSize(14)
            .fontColor(Color.White)
            .maxLines(3)
        }
        .padding(12)
        .backgroundColor('#99000000')
        .borderRadius(8)
        .margin(12)
        }
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor(this.fillsScreen() ? Color.Black : Color.Transparent)
    }
    .width('100%')
    .height('100%')
    // One rounded edge for the title bar and the app together: GTK draws
    // square corners, and the window background stays transparent.
    .borderRadius(this.maximized ? 0 : WINDOW_RADIUS)
    .clip(true)
    .expandSafeArea(this.maximized ? [SafeAreaType.SYSTEM] : [], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
    .onMouse((event: MouseEvent) => this.followTitleBarHover(event.windowY, event.action))
  }
}
