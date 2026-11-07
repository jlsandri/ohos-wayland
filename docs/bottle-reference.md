# bottle.json reference

`apps/<name>/bottle.json` defines one app. `ohos_wl.py bottle <name>` reads it, and the HAP's launcher (`app/entry/src/main/ets/pages/Index.ets`) reads its `manifest` at run time. This page lists every field either of them reads. The field names and defaults come from `app/scripts/ohos_wl.py` (`cmd_bottle`, `write_project`, `cmd_test`) and from the `AppManifest`, `AppEntry`, and `AppFile` interfaces in `Index.ets`.

## Top-level fields

`ohos_wl.py` reads these fields when it builds the bottle and the HAP project.

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `label` | string | no | The app name on the launcher icon and in the task list. It replaces `app_name` and `EntryAbility_label` in the project's string resources. The default is the bottle directory name. |
| `background` | string, CSS colour | no | The start window colour (`start_window_background` in `color.json`). Set it to the app's own background so launch and first frame look like one surface. |
| `permissions` | string array | no | Extra `requestPermissions` entries for `module.json5`. `ohos_wl.py` drops them while the HAP borrows the template bundle `com.example.myapplication`, because that profile grants no ACL permissions. |
| `programs` | object | one of `programs` or `hnp` | Libraries the direct child loads with `dlopen`, by installed file name. See [Program sources](#program-sources). |
| `hnp` | object | one of `programs` or `hnp` | Stock executables that run as their own processes, by installed name. See [Native package programs](#native-package-programs). This is the field for distribution binaries. |
| `data` | object | no | Directories to pack into `data.tar`, by path inside the archive. Each value is a path relative to the bottle directory, or `alpine:<path>` inside the unpacked Alpine root. The launcher unpacks `data.tar` into `${FILES}`. |
| `alpineBranch` | string | no | The Alpine branch packages come from. The default is the stable release `ohos_wl.py` pins (`ALPINE_STABLE`, v3.24), so builds get tested versions. Set `edge` only for apps that stable lacks, such as Ghostty; edge packages change daily. |
| `alpine` | string array | no | Alpine edge aarch64 packages to fetch before collecting programs. `hnp` programs take every library from Alpine. `programs` search Alpine first only when one of them is an `alpine:` source. |
| `icon` | string | no | The app's icon, a PNG or SVG: `alpine:<path>` (for example `alpine:usr/share/icons/hicolor/scalable/apps/<app>.svg`) or a path relative to the bottle directory. `ohos_wl.py hap` renders it with `rsvg-convert` into the HarmonyOS layered icon, centred on `iconBackground`, and into the start window icon. Without one, the icon is the label's first letter. |
| `iconBackground` | string, CSS colour | no | The layered icon's background. The default is `background`. |
| `manifest` | object | yes | The launch manifest, written to `rawfile/app.json`. See [Manifest fields](#manifest-fields). |

### Program sources

Each `programs` entry maps an installed name to a source. The installed name is the file name in the HAP `libs` directory, and `manifest.exec` refers to it.

| Source form | Resolves to |
| --- | --- |
| `native:<file>` | The template's native build output, `app/entry/build/default/intermediates/cmake/default/obj/arm64-v8a/<file>`. |
| `pool:<file>` | The library pool `app/entry/libs/arm64-v8a/<file>`, which the build scripts fill. |
| `alpine:<path>` | `<path>` inside the unpacked Alpine root, `~/.ohos-wl/alpine/root`. |
| any other string | A path relative to the bottle directory. |

`ohos_wl.py` copies each program and every `NEEDED` library that neither the runtime nor the system provides. Objects from `native:` and `pool:` stay byte-identical. Every other object is patched. It gets an `$ORIGIN` runpath, `libEGL.so` becomes `libEGL.so.1`, `libvulkan.so.1` becomes the system loader `libvulkan.so`, `libc.musl-aarch64.so.1` becomes `libc.so`, `strip --strip-unneeded` runs, and `patch_killed_syscalls.py` rewrites inline seccomp-killed syscalls, and inline ioctls the sandbox refuses fatally (`TIOCGPTPEER`), to fail the way the caller falls back from. If the closure contains `libgtk-4.so.1`, the bottle also gets `libohos_titlebar.so`, the native title bar bridge.

### Native package programs

`hnp` maps an installed name to a source, in the same forms as `programs`. Use it for stock distribution executables. They run unchanged, including non-PIE executables and Rust or C++ programs with static thread-locals, which `dlopen` cannot load.

- `ohos_wl.py bottle` copies each program with its whole Alpine library closure into `~/.ohos-wl/bottles/<name>/hnp/bin`. Only the runtime's roots (Mesa, Wayland, the exec shims, and the runtime's `libGL.so.1`) are shared. Every other library comes from Alpine, even when the system or the pool has the same soname, because older builds of the same names lack symbols stock programs need.
- `ohos_wl.py hap` packs that directory as the private native package `wlapp` 1.0, declares it in `module.json5` (`hnpPackages`), appends it to the HAP, and signs the HAP with OpenHarmony's `hap-sign-tool.jar`. hvigor's signer cannot sign the executables inside a native package. The jar is fetched at a pinned hash. Signing needs `java` on the host (`brew install openjdk@21`) and `node`, which reads the passwords DevEco Studio stored with the signing config.
- The system installs the package at `/data/app/wlapp.org/wlapp_1.0/bin`, labelled so the app may exec it. The manifest names a program as `hnp:<name>`.

The direct child forks and execs the program, with the HNP `bin` first in `LD_LIBRARY_PATH` and `libohos_exec.so` preloaded. The preload answers the seccomp-killed syscalls with `ENOSYS`, falls back for `fstat` on a memfd, supplies the few newer musl entry points the HarmonyOS libc lacks, gives the app's uid a passwd entry (user `ohos`, shell `/system/bin/sh`), and loads the title bar bridge into GTK 4 programs. Zink in the program uses `libohos_vkwsi.so`, which presents through the host's window ([architecture](architecture.md#programs-from-the-native-package)). Programs the exec'd program starts inherit all of this, so a terminal's shell or `ssh` from the same package works too.

## Manifest fields

The launcher reads these fields from `rawfile/app.json` on each launch.

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `exec` | string | required | The installed name of the library to start, or `hnp:<name>` for a program from `hnp`. |
| `entry` | string | `main` | The symbol to call. An executable that exports no `main` is entered at `_start` (`runtime/elf_entry.cpp`). Ignored for `hnp:` programs. |
| `args` | string array | `[]` | Arguments. Variables are expanded. |
| `env` | string array | `[]` | `KEY=value` entries added after the base and GPU environments, so they override both. Variables are expanded. |
| `gpu` | boolean | `true` | `false` starts the program as a plain native child with no direct GPU environment and no window handoff. Such a program draws through `wl_shm`. |
| `files` | array of [file entries](#file-entries) | `[]` | Text files to write before launch. |
| `setup` | string array | `[]` | Runtime services. The one value is `ssh-key`, which creates the key pair `${CACHE}/ghostty_hap_rsa` for the HiShell sshd and logs the public key as `OHOS_WL_SSH_PUBLIC_KEY`. |
| `decorations` | string | unset | `client` hides the HarmonyOS title bar from the start, for apps that draw their own. Any other value, or none, waits for the app's first frame to decide (`compositor/xdg_decoration.cpp`). |
| `titlebarMode` | string | `fixed` | How the native title bar shows in a maximized window. `fixed` keeps it at the top, above the app. `hover` gives the app the whole window and overlays the bar while the status bar shows. The overlay does not animate in step with the status bar. |
| `topBar` | boolean | false | The app draws a bar along its top edge while it fills the screen, such as Zed's title bar. The bar reaches the screen edge instead of keeping the top corner inset. The left, right, and bottom insets stay. The app also gets no native title bar, and its window goes fullscreen when maximized. Leave it unset for other apps, which get the native title bar in maximized mode. |
| `background` | string, CSS colour | `#000000` | The startup colour, shown until the app presents its first frame. |
| `titlebar` | object | `{}` | Native title bar placements by icon name. See [Title bar overrides](#title-bar-overrides). |
| `titlebarIcons` | object | `{}` | Native title bar icon aliases by icon name. See [Title bar overrides](#title-bar-overrides). |
| `tests` | object | unset | Named programs for a testing HAP. See [Tests](#tests). |
| `defaultTest` | string | unset | The test to run when the app starts without `--ps test`. |
| `name` | string | set by `ohos_wl.py` | The bottle name. Do not set it. |
| `data` | string array | set by `ohos_wl.py` | `["data.tar"]` when the bottle has `data`. Do not set it. |

### Variables

The launcher expands these variables in `args`, `env`, and each file's `path` and `text`. It does not expand `exec` or `entry`.

| Variable | Value |
| --- | --- |
| `${LIBS}` | The HAP `libs` directory, `<bundleCodeDir>/libs/arm64`. |
| `${HNP}` | The native package's `bin`, `/data/app/wlapp.org/wlapp_1.0/bin`. Use it to name a helper program, such as a terminal's `command`. |
| `${FILES}` | The ability's files directory. `runtime.tar` and `data.tar` unpack here. |
| `${CACHE}` | The application cache, `/data/storage/el2/base/cache`. Logs, captures, the SSH key, and Mesa's shader cache live here. |
| `${HOME}` | `${FILES}/home`. It is also `HOME` in the environment. |
| `${RUNTIME_DIR}` | `${FILES}/wayland`. It is also `XDG_RUNTIME_DIR`, and it holds the `ohos-wayland-0` socket. |

### File entries

| Field | Type | Meaning |
| --- | --- | --- |
| `path` | string | The file to write. Missing parent directories are created. |
| `text` | string | The content. |
| `ifMissing` | boolean | Write only if the file does not exist. Use it for user settings the app may change. Without it, the file is rewritten on every launch. |
| `ensureKeys` | object | Applies only with `ifMissing` when the file exists. For each key the file does not name, the launcher inserts `"key": value` after the first `{`. Each value is inserted as raw JSON text, so `"false"` becomes `false`. |

### Title bar overrides

These fields apply only to GTK 4 apps that the bridge can describe (see [the native title bars design](native-title-bars.md)). An icon name here is the freedesktop name without its `-symbolic`, `-symbolic-ltr`, or `-symbolic-rtl` suffix.

`titlebar` maps an icon name to a slot. The slots are `lead`, `afterTitle`, `center`, `actions`, and `menu`. A placement here wins over the role rules in `slotOf` (`app/entry/src/main/ets/common/NativeTitleBar.ets`).

`titlebarIcons` maps an icon name to another icon name. The title bar then draws the symbol for the new name from `app/entry/src/main/ets/common/SymbolMap.ets`. Aliases also apply inside switcher options. `tab-overview` exists only as an alias target, drawn as `sys.symbol.rectangle_stack`.

### Tests

Each `tests` entry has the [program fields](#manifest-fields) `exec`, `entry`, `args`, `env`, and `gpu`, plus these fields that `ohos_wl.py test` reads:

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `expect` | string | unset | The test passes when this string appears in `cache/ohos_wl_app:testing:<test>.log` or in hilog since launch. With no `expect`, the test always passes. |
| `seconds` | number | `12` | The deadline. `ohos_wl.py test` checks for `expect` once a second and fails the test when the deadline passes. |
| `foreground` | boolean | `false` | Starts the test with `aa start`, rather than the hidden `aa pre-start`. |
| `fullscreen` | boolean | `false` | Starts in the foreground, then calls `maximize(ENTER_IMMERSIVE)`. |
| `maximize` | boolean | `false` | Runs hidden with the app and its client behaving as maximized, without calling `window.maximize()`. With `OHOS_WL_TEST_FOREGROUND=1` it starts in the foreground and calls `maximize(EXIT_IMMERSIVE)`. Use this to test the custom overlay. |
| `titlebarMode` | string | the bottle's value | Overrides the bottle's `titlebarMode` for this test. |
| `replay` | string | unset | A recorded input file to feed through the launcher's input path. Variables are expanded. |
| `reject` | string array | `[]` | Any listed string in the output fails the test, even after `expect` matches. |
| `edges` | boolean | `false` | Also captures the frame and fails unless the inset margins continue the app's edge pixels. Use with `maximize`. |
| `manual` | boolean | `false` | Excludes the test from a run with no named tests. |

Choose at most one of `fullscreen` and `maximize`. `aa start -a EntryAbility -b <bundle> --ps test <name>` runs one test and replaces the running program. Other tests use `aa pre-start` (API 26), which keeps the window hidden. The launcher reads each test as a `test <name>` line in `cache/ohos_wl_request`. `OHOS_WL_TEST_FOREGROUND=1` uses `aa start` for every test. The testing app closes when the run finishes.

## Base environment

Every program starts with this environment, before the GPU and manifest entries (`baseEnvironment` in `Index.ets`):

```
XDG_RUNTIME_DIR=${RUNTIME_DIR}
WAYLAND_DISPLAY=ohos-wayland-0
HOME=${HOME}
XDG_DATA_DIRS=${FILES}/share:${FILES}/runtime/share
XDG_CONFIG_HOME=${HOME}/.config
XDG_CONFIG_DIRS=${FILES}/runtime/etc
MESA_SHADER_CACHE_DIR=${CACHE}/mesa_shader_cache
ADW_DEBUG_COLOR_SCHEME=prefer-dark or prefer-light, from the system colour mode
FONTCONFIG_FILE=${FILES}/runtime/fonts.conf
XKB_CONFIG_ROOT=${FILES}/runtime/share/X11/xkb
XLOCALEDIR=${FILES}/runtime/share/X11/locale
XCURSOR_PATH=${FILES}/runtime/share/icons
XCURSOR_THEME=Adwaita
GDK_BACKEND=wayland
GSK_RENDERER=gl
GTK_A11Y=none
```

GPU programs then get `GraphicsBroker::GuestEnvironment` (`app/entry/src/main/cpp/graphics/graphics_broker.cpp`), which selects Zink on the direct window path. [The architecture document](architecture.md#runtime-gl-environment) lists those entries.

## Example

A stock Alpine terminal and the `ssh` it runs, with no patches. The SSH options are shortened here:

```json
{
	"label": "Ghostty",
	"alpine": ["ghostty", "openssh-client-default"],
	"hnp": {
		"ghostty": "alpine:usr/bin/ghostty",
		"ssh": "alpine:usr/bin/ssh"
	},
	"data": {
		"share/ghostty": "alpine:usr/share/ghostty",
		"share/terminfo": "alpine:usr/share/terminfo"
	},
	"manifest": {
		"exec": "hnp:ghostty",
		"env": ["GHOSTTY_RESOURCES_DIR=${FILES}/share/ghostty", "TERMINFO_DIRS=${FILES}/share/terminfo"],
		"setup": ["ssh-key"],
		"files": [
			{
				"path": "${HOME}/.config/ghostty/config",
				"text": "command = ${HNP}/ssh -tt -i ${CACHE}/ghostty_hap_rsa -p 8022 ohos@127.0.0.1 zsh -l\n"
			}
		],
		"decorations": "client",
		"background": "#282c34"
	},
	"background": "#282c34"
}
```

A JIT runtime such as Node runs with its JIT off (`"env": ["NODE_OPTIONS=--jitless"]`). HarmonyOS grants JIT only to its own JSVM. V8's JIT still failed under a fresh Oniro profile for an app declaring both `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` and `DISABLE_CODE_MEMORY_PROTECTION`.

A bundle with no cached signing profile gets one from `oniro-app sign --harmonyos` on its first `hap` (run `oniro-app auth login` once). `ohos_wl.py` points Oniro at `~/.ohos-wl/sdk`, links to the command-line tools' SDK plus `hap-sign-tool.jar`, which those tools lack.
