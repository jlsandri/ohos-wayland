# ohos-wayland

ohos-wayland runs aarch64 Linux Wayland apps as ordinary HarmonyOS apps on a HarmonyOS 7 PC. Each HAP contains a Wayland compositor, Mesa, and one app's files. The app renders through Zink and the system Vulkan driver. It presents into a HarmonyOS window without a VM, an X server, or a remote desktop.

The repository contains the shared runtime, the `ohos_wl.py` packager, the native title bar protocol and GTK bridge, and a testing bottle. Apps arrive as prebuilt binaries. The runtime and packager need no app-specific patches.

![A GTK 4 app with a native HarmonyOS title bar](evidence/gtk-demo.png)

## What works

- Wayland clients render through Mesa EGL and Zink. In-process clients present without CPU frame copies.
- Stock executables run from the HAP's private native package. Their presentation path is in [the architecture document](docs/architecture.md#presenting-from-another-process).
- GTK 4 header bars become native HarmonyOS title bars. Buttons and menus keep their GTK actions.
- A maximized window keeps the native title bar at the top of the window. A bottle can choose a hover overlay with `titlebarMode`.
- Keyboard, mouse, touch, touchpad scrolling, clipboard, and HarmonyOS drag and drop share the same runtime path across apps.
- The diagnostics overlay shows presentation rate, frame pacing, input latency, buffer geometry, and memory use. Press **Ctrl+Alt+Shift+D** to toggle it.

![The live diagnostics overlay over a GTK 4 app](evidence/diagnostics-overlay.png)

## Requirements

You need these tools on a HarmonyOS PC in free-window mode:

- HarmonyOS 7 with API 26 and a Vulkan driver that exposes `VK_OHOS_surface`.
- HiShell and Harmonybrew. The build scripts install their own dependencies with `brew`.
- The HarmonyOS command-line tools, including the SDK, `hvigor`, and `hdc`.
- `devecocli` and `oniro-app`. Oniro signs debug profiles for CN, SG, EU, and RU accounts.
- A Huawei developer account. Each user signs HAPs with their own debug profile.
- Python, Meson, Ninja, pkg-config, patchelf, binutils, OpenJDK 21, Node, `librsvg`, Rust, and Cargo.

Set the paths for your installation. Replace the example HDC address with your device's address.

```sh
export TOOL_HOME=/path/to/command-line-tools
export COMMAND_LINE_TOOL_PATH="$TOOL_HOME"
export HDC="$TOOL_HOME/sdk/default/openharmony/toolchains/hdc"
export OHOS_WL_DEVICE=127.0.0.1:40417
export DEVECOCLI=/path/to/devecocli
export ONIRO=/path/to/oniro-app
```

## Build and run the testing app

Run every command from this directory in HiShell.

1. Fetch the pinned external sources and apply the patch series. This step is idempotent.

	```sh
	sh app/scripts/fetch_sources.sh
	```

2. Build the shared GPU and GTK libraries.

	```sh
	mkdir -p build/logs
	sh app/scripts/build_gpu.sh > build/logs/gpu-build.log 2>&1
	sh app/scripts/build_gtk.sh > build/logs/gtk-build.log 2>&1
	python3 app/scripts/stage_gtk.py > build/logs/stage.log 2>&1
	```

3. Build the testing probes.

	```sh
	sh tools/wl-gl-probe/build.sh
	sh tools/shm-probe/build.sh
	sh tools/syscall-probe/build.sh
	sh tools/scroll-probe/build.sh
	sh tools/hover-probe/build.sh
	sh tools/tls-probe/build-musl.sh
	```

4. Build the shared runtime.

	```sh
	python3 app/scripts/ohos_wl.py runtime
	```

5. Sign in to AppGallery Connect once.

	```sh
	oniro-app auth login
	```

	The first HAP build asks for a bundle prefix. Use a prefix that belongs to your developer account. The packager generates missing debug profiles and caches them in `~/.ohos-wl/signing`.

6. Package, sign, and install the testing app. Then run its device tests.

	```sh
	python3 app/scripts/ohos_wl.py bottle testing
	python3 app/scripts/ohos_wl.py hap testing
	python3 app/scripts/ohos_wl.py install testing
	python3 app/scripts/ohos_wl.py test
	```

	Every test prints `PASS` or `FAIL`. Run one test with `python3 app/scripts/ohos_wl.py test gtk-demo`.

	`hover-probe` is a manual check for real pointer delivery. It can request input-injection authorization. The default test suite does not run it.

## Add an app

Create `apps/<name>/bottle.json`. Start with a prebuilt aarch64 musl binary from Alpine Linux. List its packages, executables, data directories, and launch settings. [The bottle reference](docs/bottle-reference.md) documents every field.

Run the inner loop to package, sign, install, and start the app.

```sh
python3 app/scripts/ohos_wl.py dev <name>
```

Fix compatibility problems in the runtime, compositor, launcher, or packager. Do not add app-specific branches. Check each fix on at least two apps or on an app and a test.

## Inspect an app

- `python3 app/scripts/ohos_wl.py run <name>` starts an app.
- `python3 app/scripts/ohos_wl.py stop <name>` closes it.
- `python3 app/scripts/ohos_wl.py screenshot <name> --window` captures only its HarmonyOS window, including the native title bar.
- `python3 app/scripts/ohos_wl.py screenshot <name>` captures its next composed client frame.
- Program output goes to hilog and to `cache/ohos_wl_<app id>.log` in the app sandbox.

## Source-only releases

GitHub releases contain source code only. They contain no HAPs or prebuilt runtime archives. HarmonyOS requires each user to sign HAPs with a debug profile that belongs to that user's developer account.

## Known limits

- An app may execute `/system/bin` programs and programs in its own native package. Bottle every helper the app starts.
- Stock executables copy presented images into host-owned window buffers. In-process clients do not copy frames.
- Only the newest toplevel and its popups appear. A second window from the same app is not presented.
- The first buffer after a window handoff can fail `OH_NativeImage_UpdateSurfaceImage` with `60002000`. An app that draws once and then waits may stay blank.
- Native title bars cover GTK 4. A header bar with a custom popover or an unknown widget falls back to a styled GTK header bar.
- A hover title bar does not animate in step with the status bar, because no HarmonyOS window API reports the status bar.
- The project has been tested on a small number of devices and GPUs. New devices need runtime verification.

## Repository layout

| Path | Contents |
| --- | --- |
| `app/` | The runtime template HAP and build scripts. |
| `app/scripts/ohos_wl.py` | The runtime and app packager. |
| `apps/testing/` | The testing bottle and input fixtures. |
| `tools/` | Probe programs for device tests. |
| `protocols/` | The native title bar protocol. |
| `patches/` | Patch series for platform libraries. |
| `docs/` | Architecture, title bar design, and the bottle reference. |
| `evidence/` | Reviewed app-window captures. |

## Licence

Licences follow the code's origin by directory. WineHua-derived runtime code is LGPL-2.1-only. Project-owned packager, tools, bottles, documents, and new runtime files are MIT. Generated protocol code and patch series keep their upstream licences.

See `REUSE.toml`, `LICENSES/`, and `NOTICE` for the exact assignments and credits.
