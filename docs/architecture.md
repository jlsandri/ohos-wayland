# Native Wayland architecture

## Goal

Run native arm64 Wayland applications on HarmonyOS 7 without a VM, Wine, Box64, or streamed frames.

## Current status

Each app has its own HAP built from one runtime template. Stock distribution executables run unchanged from the HAP's native package. Alpine's Ghostty with its `ssh`, `weston-simple-egl`, ripgrep, and Node all run with no patches. The testing bottle covers GPU presentation, shared memory, input, clipboard, and drag and drop.

## Frame path

`Wayland client -> Mesa EGL (Wayland platform) -> Zink -> Maleoon Vulkan -> Kopper (VK_OHOS_surface) -> compositor-owned OHNativeWindow -> OH_NativeImage -> external OES texture -> XComponent`

GTK clients reach Mesa EGL through libepoxy. wgpu clients such as Zed call Mesa EGL directly. The client owns its rendering. The compositor owns the window the client presents into. In-process clients present without CPU frame copies. Stock executables use the host-buffer path described below.

## Packaging

Packaging follows Harmonybrew. The runtime is built once, apps arrive as prebuilt files, and nothing is compiled per app. `app/scripts/ohos_wl.py` has three layers.

The runtime is the part every app shares. `ohos_wl.py runtime` builds the template HAP in `app/` to compile the native code, then copies the closure of `libentry.so`, `libohos_direct_child.so`, Mesa, Wayland, and xkbcommon (15 libraries) into `~/.ohos-wl/runtime/lib`. It also writes `runtime.tar` with the xkb data, the X11 compose locale, the GLib schemas, the hicolor and Adwaita icon themes (Adwaita brings the cursors), and a `fonts.conf` that points at `/system/fonts`.

A bottle is one app's files. `apps/<name>/bottle.json` names programs, data directories, Alpine packages, a launch manifest, a label, and permissions. `ohos_wl.py bottle <name>` copies each program and every library it needs that neither the runtime nor the system provides into `~/.ohos-wl/bottles/<name>`. A program source is a path, `native:` (the template's native build output), `pool:` (`app/entry/libs/arm64-v8a`, filled by the build scripts), or `alpine:`. Data directories go into `data.tar`.

`fix_object` patches only foreign objects. Objects from the native output and the pool stay byte-identical, because rewriting `libentry.so` with patchelf and strip stopped its NAPI module from registering, and rewritten Mesa objects made every GL client exit at start. A foreign object gets:

- An `$ORIGIN` runpath.
- `libEGL.so` mapped to Mesa's `libEGL.so.1`, because plain `libEGL.so` is the system driver.
- Alpine's `libc.musl-aarch64.so.1` mapped to the system `libc.so`.
- `strip --strip-unneeded`.
- Inline seccomp-killed syscalls rewritten by `patch_killed_syscalls.py`.

Archives dereference symlinks, because app storage refuses them.

A HAP is the runtime plus one bottle. `ohos_wl.py hap <name>` copies the template's ArkTS, resources, and hvigor files into `build/haps/<name>` and sets the bundle name to `com.deviceexploration.<name>`. It removes `externalNativeOptions`, so the HAP build packages the prebuilt libraries without running CMake. The runtime and bottle libraries go into `entry/libs/arm64-v8a`. `runtime.tar`, `data.tar`, and the bottle's `app.json` go into `rawfile`. Bottle permissions are added to `module.json5`.

## Launcher

`entry/src/main/ets/pages/Index.ets` is one launcher for every HAP. It reads `rawfile/app.json`, which names the program library, its entry symbol (`main` by default), arguments, environment, data tars, text files to write, and setup steps. A testing manifest also names tests.

On launch the launcher:

1. Unpacks `runtime.tar` and each data tar into the files directory. A stamp file holds the installed `versionCode` and `updateTime`, so each archive unpacks once per installed version. The native `extractTar` (`runtime/tar_extract.cpp`) reads the archive straight from the HAP through `ResourceManager.getRawFdSync`.
2. Expands `${LIBS}`, `${FILES}`, `${CACHE}`, `${HOME}`, and `${RUNTIME_DIR}` in paths, file contents, arguments, and environment values. `${CACHE}` is the application cache, `/data/storage/el2/base/cache`.
3. Writes the manifest's text files. Ghostty's config is one of them.
4. Runs the setup steps. `ssh-key` creates the HiShell login key in the cache.
5. Starts the program in a native child with the base environment (`XDG_DATA_DIRS`, `XKB_CONFIG_ROOT`, `XLOCALEDIR`, `XCURSOR_PATH`, `FONTCONFIG_FILE`, and the GDK settings), the runtime GL environment, and the manifest's environment.

`aa start -a EntryAbility -b <bundle> --ps test <name>` runs one named test, which replaces the running program. Tapping the icon of a running app only brings it to the front. `--ps capture 1` asks the renderer to write its next composed frame to `cache/ohos_wl_capture.ppm` and does not restart the app. The capture reads back one frame with `glReadPixels` for diagnostics and is not on the presentation path.

## Runtime GL environment

`GraphicsBroker::GuestEnvironment` (`graphics/graphics_broker.cpp`) gives every GPU app the same Mesa settings, so bottles carry no GL settings:

- `GALLIUM_DRIVER=zink`, `EGL_PLATFORM=wayland`, `ZINK_VULKAN_LIBRARY=libvulkan.so`, and `OHOS_WAYLAND_DIRECT_WINDOW=1` select the direct path.
- `MESA_GL_VERSION_OVERRIDE=4.3` and `MESA_GLSL_VERSION_OVERRIDE=430`. Zink derives GL 4.0 on the Maleoon, because GL 4.1 and later need 16384-pixel textures (the GPU has 8192), BPTC, and robust buffer access. wgpu needs GLSL 430 for compute. The real limits stay queryable.
- `ZINK_READONLY_VERTEX_SSBO=1`. The Maleoon driver lacks `vertexPipelineStoresAndAtomics`, so the Mesa fork decorates read-only vertex storage buffers `NonWritable` and exposes them only when this variable is set.
- `MESA_EXTENSION_OVERRIDE=-GL_ARB_blend_func_extended -GL_EXT_blend_func_extended`. Dual-source blending renders wrong on the Maleoon, and gpui's subpixel text came out as hollow, colour-fringed outlines. Without the extension, apps fall back to grayscale antialiasing.

## Processes

The app process runs the ArkTS launcher, the Wayland compositor, the XComponent renderer, and the NativeImage consumer.

Each direct-window client runs in its own native child process. `GraphicsBroker::LaunchDirect` starts it with `OH_Ability_CreateNativeChildProcess("libohos_direct_child.so")`. That host does three jobs:

- It receives the `OWP1` launch record over IPC and starts the app.
- It keeps a registry from surface key to the `OHNativeWindow` the compositor attaches (`OhosDirectWindowAcquire`).
- It forwards the program's stdout and stderr to HiLog and to `cache/ohos_wl_<app id>.log`.

A manifest entry with `"gpu": false`, such as the `shm-demo` test, runs as a plain native child process (`OH_Ability_StartNativeChildProcess`). Such a child has no IPC channel, so it cannot receive a window and draws through `wl_shm`.

## App entry

The host loads the app with `dlopen` from the HAP `libs` directory. For an NCP-style app it calls the named entry point. For an executable it applies the launch environment and patches the app's imports (`runtime/elf_entry.cpp`):

- `fstat` and `fstat64` fall back to `lseek` when the kernel returns `EACCES` for a memfd.
- `syscall` returns `ENOSYS` for the syscalls that seccomp kills.
- `exit`, `_exit`, `_Exit`, and `abort` log `DIRECT_APP_EXIT` with the caller's offset, then end the process.

The host then calls the executable's exported `main`. If there is none, it builds an initial process stack (argc, argv, envp, and an empty auxv) on a fresh 8 MiB mapping and jumps to `_start`. `__libc_start_main` is redirected to a hook that calls `main` and jumps back to the host with its status.

## Programs from the native package

A distribution executable cannot always be loaded with `dlopen`. A non-PIE executable needs a fixed address the Ark runtime already holds in the host, and musl gives a late-loaded object no static thread-local block, so Rust and C++ programs with local-exec or initial-exec thread-locals break. The HAP's `libs` files also cannot be exec'd. Their SELinux label allows mapping but not `execve`.

A bottle's `hnp` programs therefore ship in the HAP's private native package (HNP). The installer unpacks it to `/data/app/wlapp.org/wlapp_1.0/bin` with the label `private_hnp_file`, which the app's domain may exec. For a manifest `exec` of `hnp:<name>`, the launcher starts the direct child with entry `exec`, and the host forks and execs the program (`RunExec` in `runtime/direct_child.cpp`). The kernel and the system musl load it as the main program, so every executable kind runs unchanged. The host stays as its parent, forwards its output, and ends it when the host ends (`PR_SET_PDEATHSIG`).

Three runtime libraries stand in for what the in-process path does:

- `libohos_exec.so` is preloaded (`runtime/exec_preload.cpp`). It answers the seccomp-killed syscalls, falls back for `fstat` on a memfd, adds the newer musl symbols the HarmonyOS libc lacks (`__res_state`, `pidfd_getpid`, `pidfd_spawnp`), gives the app's uid a passwd entry, stubs the Wayland and X11 Vulkan surface entry points the system loader lacks, and loads the title bar bridge into GTK 4 programs.
- `libohos_vkwsi.so` is the Vulkan library Zink loads in the program (`runtime/remote_wsi_client.cpp`). It forwards to the system loader except for the OHOS surface and swapchain.
- `libGL.so.1` (`runtime/libgl_alias.c`) depends on Mesa's `libGLESv2.so.2`. libepoxy and GStreamer GL take desktop GL entry points through it, so they reach the runtime's Mesa instead of loading a distribution Mesa.

### Presenting from another process

An exec'd process has no IPC channel to the app, so it cannot receive an `OHNativeWindow`. The Maleoon driver also shares images between processes only as `OH_NativeBuffer`s (a probe found only the OHOS native buffer handle types exportable; opaque fd and dma-buf are refused), and those need IPC too. The host therefore lends the window's own buffers:

1. Mesa's direct path calls `OhosDirectWindowAcquire`, which the preload answers with a token for the surface key.
2. Zink's swapchain images are ordinary images in `libohos_vkwsi.so`. Creating the swapchain connects to the host's socket (`OHOS_WAYLAND_WSI_SOCKET`). The host looks up the window the compositor attached for that key and sets its geometry, RGBA format, and CPU usage (`runtime/remote_wsi_host.cpp`).
3. On present, the app's thread queues a copy of the image into a host-visible buffer. A worker thread waits for it, asks the host for a window buffer (its dma-buf fd arrives once per buffer), copies the rows into the mapped buffer, and asks the host to flush it.

The compositor keys surfaces by the client's pid. The broker resolves an exec'd pid to its host by walking `/proc/<pid>/stat` parents (`ProxyForKeyLocked`).

Each frame costs one GPU copy and one CPU copy, off the app's render thread. Stock `weston-simple-egl` at 2090x1324 runs at 117 to 120 fps on the 120 Hz panel (`VKWSI_TIMING`, with the copy at 2.7 to 2.9 ms per frame). The compositor's loop takes about 2.5 ms of each 8.3 ms period (`[EGL] loop timing`). The in-process path does not copy.

## Window handoff

1. The client creates an `xdg_toplevel`. Its first configure carries the window's logical size. With 0x0, gpui picked 4096x1324 logical and allocated an 8192-wide first buffer.
2. The compositor publishes the surface key `pid << 32 | wl_surface id`. `GpuFrameConsumer` creates an `OH_NativeImage` and takes its producer window. The broker parcels that window to the client's host with `kAttachSurfaceRequest`.
3. The client creates its EGL window surface. With `OHOS_WAYLAND_DIRECT_WINDOW=1`, Mesa's Wayland platform fetches the window from the host and gives Kopper a `VkSurfaceCreateInfoOHOS` in place of a Wayland surface.
4. Mesa commits the `wl_surface` after each swap, with no `wl_buffer`. The commit carries full-buffer damage, which the compositor reads as the buffer size. The surface's logical size is the `wp_viewport` destination when one is set, which is how GTK scales, and otherwise the buffer size over the buffer scale.
5. The toplevel maps and takes focus on its first frame, not on the empty initial commit that requests the first configure. A client that sets no window geometry is mapped at the size the compositor configured.
6. The compositor holds the commit's frame callbacks. The renderer releases them on each vsync tick. If no tick comes for 100 ms, as when the window is hidden, the Wayland thread releases them itself. The renderer fences each frame with `EGL_KHR_fence_sync`.

## Window decorations

`compositor/xdg_decoration.cpp` implements `zxdg_decoration_manager_v1`. The app draws its own title bar unless it requests server-side mode. Clients without the protocol, such as GTK 4, count as client-side when their first frame sets a window geometry, because GTK sets one exactly when it draws CSD shadows.

The compositor sends the result to ArkTS as a `decoration` toplevel event. A client-side app hides the HarmonyOS window decor with `setWindowDecorVisible(false)`, and a server-side app keeps it. The client's title bar drives the HarmonyOS window. Its `move_start`, maximize, unmaximize, and minimize requests map to the matching window calls, and its title becomes the window title. When the app destroys its own toplevel, the launcher stops the program and closes the HarmonyOS window. A toplevel the launcher stopped itself, as when a test replaces the program, closes without closing the window.

## Scene and input

`DirectScene` (`graphics/direct_scene.cpp`) holds the presented scene. The scene is the newest toplevel plus its `xdg_popup` stack, bottom to top, with popup positions relative to their parent's window geometry. Removing a popup removes the popups stacked on it. The renderer draws each layer from the scene snapshot.

The single `wl_output` reports scale `round(density)`. Every surface receives `wl_surface.enter` for the client's outputs on its first commit, because clients such as gpui take their scale from the outputs a surface has entered. The output resends `wl_output.mode` when the window size changes. GTK clamped windows to the stale monitor size before. Configure sizes are logical.

Input hit-tests the scene topmost first. A popup takes input over its window geometry, and the toplevel takes the rest. The point arrives in the hit surface's local coordinates.

One EGL context and one consumer outlive individual renderers, because a NativeImage stays bound to the context it was created in.

## Signing

A HAP with a native package is signed by OpenHarmony's `hap-sign-tool.jar` (Apache-2.0, fetched at a pinned hash). hvigor's signer leaves the executables inside the package unsigned, and the install then fails with 9568407. The jar takes the signing passwords in plain text, so `app/scripts/deveco_password.js` decrypts the ones stored with the config, using the scheme TermonyHQ/Termony's `sign.js` (MIT) describes.

Every bundle name needs its own debug signing profile. Run `oniro-app sign --harmonyos --product default` from `build/haps/<name>` after authenticating once with `oniro-app auth login`. Oniro supports the host's SG account and writes the project's `signingConfigs`. `ohos_wl.py` then reads the config from the project and caches it in `~/.ohos-wl/signing/<bundle>.json`. `devecocli build` and `devecocli run` can use the generated config, but `devecocli` does not share Oniro's session and its own login and signing commands accept only CN accounts.

A debug profile carries every permission the module declares once regenerated, including `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`. The template profile for `com.example.myapplication` has `"allowed-acls":[]`. Installing a HAP that declares an ACL permission under it fails with 9568289. `OHOS_WL_BUNDLE=com.example.myapplication` borrows that profile for any app, and `ohos_wl.py` then drops the bottle's permissions.

## Platform facts this depends on

- `VK_OHOS_surface` comes from the system loader's implicit layer `VK_LAYER_OHOS_surface` (`libvulkan_swapchain.so`). Zink loads the system `libvulkan.so` through `ZINK_VULKAN_LIBRARY`.
- OHOS has no Vulkan Wayland WSI and no DRM node. In direct mode Mesa skips the DRM requirements and uses the software EGLDevice.
- The OHOS swapchain labels every buffer RGBA. In direct mode Mesa offers only RGBA-ordered configs, so red and blue are not swapped.
- The HarmonyOS loader maps code only from paths the app's linker namespace permits, which is the HAP `libs` directory. App processes may not create namespaces. Apps therefore ship inside their HAP.
- App storage refuses symlinks. Data archives hold real files.
- An app process may exec `/system/bin` binaries and its private native package, not the HAP `libs` directory or `/lib/ld-musl-aarch64.so.1`. Stock programs and the helpers they start run from the native package.
- seccomp in the app sandbox kills the process on some newer syscalls instead of failing them. `runtime/sandbox_syscalls.h` lists them.
- Executable anonymous memory needs `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`. Without it JIT compilers such as wasmtime's fail. V8's JIT still fails with it, even when the app also declares `DISABLE_CODE_MEMORY_PROTECTION` under a fresh profile. HarmonyOS documents JIT only for its own JSVM, so Node runs with `--jitless`.
- musl refuses to `dlopen` an object with initial-exec TLS that resolves to another object (`initial-exec TLS resolves to dynamic definition`), and a `dlopen`ed executable's local-exec thread-locals read the host's. Such programs run from the native package instead.

## Ghostty

`apps/ghostty/bottle.json` packages Alpine's Ghostty and OpenSSH client unchanged, as `hnp` programs. The app sandbox has no user shell or Harmonybrew tools, so the bottle writes a Ghostty config whose `command` runs the bundled `ssh` against the HiShell sshd on port 8022 with the key from the `ssh-key` setup step. `ohos_wl.py authorize ghostty` adds that key to the host `~/.ssh/authorized_keys`. The earlier source build (`app/scripts/build_ghostty.sh`, with a `main_ohos.zig` entry and an in-process SSH library) is no longer needed.

## Rust apps and Zed

Rust apps target `aarch64-unknown-linux-musl` with std built from source (`-Z build-std`), dynamic libc (`-C target-feature=-crt-static`), and `-Z tls-model=global-dynamic`. The `ohos` target is wrong for a Wayland client, because crates that branch on `target_env = "ohos"` expect a native HarmonyOS app.

The final crate must be a `cdylib` that exports a C `main`. A binary crate is compiled as a PIE, and LLVM then uses initial-exec TLS for thread-locals from other crates whatever `-Z tls-model` says. `tools/tls-probe/build-musl.sh` is the minimal recipe, and `app/scripts/build_zed.sh` builds Zed this way.

Zed renders through wgpu's GL backend on Mesa EGL and Zink. The Mesa fork adds two changes for wgpu:

- Every OHOS direct-window config carries `EGL_PBUFFER_BIT`. wgpu rejects configs that allow only windows, because it renders off-screen through surfaceless contexts.
- Zink decorates storage buffers in vertex stages `NonWritable` when the shader never writes memory. `ZINK_READONLY_VERTEX_SSBO=1` exposes them on drivers without `vertexPipelineStoresAndAtomics`, such as the Maleoon 916B.

## GTK and Mesa builds

`app/scripts/build_gtk.sh` builds GTK 4.20.3, libadwaita 1.8.3, and gtk4-layer-shell 1.3.0 for Wayland only. GTK uses `libguest_epoxy.so.0`, which resolves desktop GL through Mesa's `libEGL.so.1`. With `OHOS_WAYLAND_DIRECT_WINDOW` set, GDK requests `EGL_OPENGL_BIT` configs.

`app/scripts/build_gpu.sh` builds Wayland, libdrm, guest libepoxy, and Mesa with the Wayland and OHOS EGL platforms and the Zink Gallium driver. Mesa builds desktop GL and GLES 2 and 3 (`-Dopengl=true -Dgles2=enabled`). It ships as `libEGL.so.1`, because plain `libEGL.so` is the system driver.

## Main data shapes

`bottle.json` is one app's packaging input. It holds `label`, `permissions`, `programs` (installed library name to source), `data` (archive path to directory), `alpine` packages, and `manifest`.

`AppManifest` (`Index.ets`) is the bottle's `manifest` as written to `rawfile/app.json`. It holds `name`, `exec`, `entry`, `args`, `env`, `gpu`, `data`, `files`, `setup`, `tests`, `defaultTest`, `decorations`, `topBar`, `titlebarMode`, `titlebar`, `titlebarIcons`, and `background`. [The bottle reference](bottle-reference.md) describes each one.

`AppRecord` owns an application identifier, package path, entry point, arguments, environment, and running pids. The launcher sends it as an `OWP1` launch record.

`SurfaceInfo` identifies a present stream by client pid and Wayland surface id. `GpuFrameConsumer` owns the NativeImage, external texture, transform, and source size for the presented stream.

`DirectLayer` is one surface in the presented scene. It holds the surface key, a popup flag, the position relative to the toplevel, the window geometry, and the surface size.
