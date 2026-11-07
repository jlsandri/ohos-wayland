#!/usr/bin/env python3
"""ohos-wl: package aarch64 Wayland apps as their own HarmonyOS HAPs.

Runs on the HarmonyOS host (HiShell) with Harmonybrew. Like Harmonybrew, it
reuses prebuilt binaries: apps are not compiled here.

  ohos_wl.py runtime            build the runtime once (compositor, Mesa, data)
  ohos_wl.py bottle <name>      collect apps/<name> into a patched bottle
  ohos_wl.py project <name>     write build/haps/<name> without building it
  ohos_wl.py hap <name>         assemble, sign, and build the HAP
  ohos_wl.py install <name>     install the HAP on the device (no launch)
  ohos_wl.py run <name> [test]  start the app, or one test of a testing HAP
  ohos_wl.py test [test...]     run the testing HAP's tests and check output,
                                then close the testing app
  ohos_wl.py stop <name>        close the app
  ohos_wl.py screenshot <name> [out.png]
                                capture the app's composed frame (its window only)
  ohos_wl.py authorize <name>   accept the app's SSH key on the HiShell sshd
  ohos_wl.py deveco-sign <name...>
                                get Huawei debug profiles through DevEco Studio
  ohos_wl.py all <name>         bottle, hap, and install
  ohos_wl.py dev <name> [test...]
                                the inner loop: compile, then bottle and hap only
                                when stale, install, and run the named tests

A bottle (apps/<name>/bottle.json) names prebuilt programs, data
directories, and Alpine packages. `bottle` copies the programs and every
library they need that neither the runtime nor the system provides, patches
them for the app sandbox, and stores them in ~/.ohos-wl/bottles/<name>.
`hap` combines the runtime and one bottle into build/haps/<name>, a HarmonyOS
project with bundle name <prefix><name>; the prefix is the builder's own
(OHOS_WL_BUNDLE_PREFIX or ~/.ohos-wl/bundle-prefix, asked for once).

`programs` are loaded with dlopen into the direct child. `hnp` programs are
stock executables, such as Alpine's, that run as their own processes: `hap`
packs them with their Alpine libraries into the HAP's native package, which
the system installs where the app may exec them, and signs the HAP with
OpenHarmony's hap-sign-tool.jar (Java). The manifest names them hnp:<name>.
"""

import functools
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
APP_ROOT = SCRIPT_DIR.parent
ROOT = APP_ROOT.parent
APPS = ROOT / "apps"
HAPS = ROOT / "build" / "haps"
STORE = Path(os.environ.get("OHOS_WL_STORE", Path.home() / ".ohos-wl"))
RUNTIME = STORE / "runtime"
BOTTLES = STORE / "bottles"
SIGNING = STORE / "signing"
BREW = Path(os.environ.get("HOMEBREW_PREFIX", Path.home() / ".harmonybrew"))
TOOL_HOME = Path(os.environ.get("TOOL_HOME", "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0"))
HDC = Path(os.environ.get("HDC", TOOL_HOME / "sdk/default/openharmony/toolchains/hdc"))
DEVECOCLI = Path(os.environ.get("DEVECOCLI", Path.home() / "npm/bin/devecocli"))
ONIRO = Path(os.environ.get("ONIRO", Path.home() / "npm/bin/oniro-app"))
DEVICE = os.environ.get("OHOS_WL_DEVICE", "127.0.0.1:37757")
ALPINE_MIRROR = os.environ.get("ALPINE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
# Packages come from this Alpine stable release, so a build gets the
# versions that were tested. A bottle sets "alpineBranch": "edge" for apps
# that only edge has (Ghostty); those builds can change from day to day.
ALPINE_STABLE = "v3.24"
ALPINE_BRANCH = [ALPINE_STABLE]
BUNDLE_PREFIX_FILE = STORE / "bundle-prefix"

# Build outputs the runtime and bottles draw from: libraries staged by
# build_gpu.sh and stage_gtk.py, and the native code compiled by `runtime`.
POOL = APP_ROOT / "entry/libs/arm64-v8a"
NATIVE_OUT = APP_ROOT / "entry/build/default/intermediates/cmake/default/obj/arm64-v8a"
# The SDK sysroots list the NDK libraries the system gives every app; HiShell
# cannot see all of them under /system.
SYSTEM_DIRS = [TOOL_HOME / f"sdk/default/{kit}/native/sysroot/usr/lib/aarch64-linux-ohos"
               for kit in ("openharmony", "hms")]
SYSTEM_DIRS += [Path(p) for p in ("/system/lib64", "/system/lib64/ndk", "/system/lib64/platformsdk")]

# The compositor, the direct-GPU child, and Mesa with what it loads.
RUNTIME_ROOTS = [
    "libentry.so", "libohos_direct_child.so", "libc++_shared.so",
    "libEGL.so.1", "libGLESv2.so.2", "libgallium-25.0.1.so",
    "libwayland-client.so.0", "libwayland-egl.so.1", "libwayland-cursor.so.0",
    "libxkbcommon.so.0",
    # Preloaded into hnp programs, and the Vulkan library Zink uses there.
    "libohos_exec.so", "libohos_vkwsi.so",
    # Desktop GL entry for distribution programs, on this Mesa (libgl_alias.c).
    "libGL.so.1",
]

# Loaded into GTK apps by the direct child; draws their header bars as
# HarmonyOS title bars.
TITLEBAR_BRIDGE = "libohos_titlebar.so"

# Shared data every app gets under ${FILES}/runtime (see Index.ets).
RUNTIME_DATA = [
    (BREW / "share/X11/xkb", "share/X11/xkb"),
    (BREW / "share/X11/locale", "share/X11/locale"),
    (BREW / "share/icons/hicolor", "share/icons/hicolor"),
    (APP_ROOT / "native-build/gtk-install/share/glib-2.0/schemas", "share/glib-2.0/schemas"),
]
RUNTIME_ALPINE = {"adwaita-icon-theme": [("usr/share/icons/Adwaita", "share/icons/Adwaita")]}
# HarmonyOS fonts, GTK style and window-control glyphs for every app
# (fonts.conf, gtk-4.0/, icons/).
RUNTIME_STYLE = APP_ROOT / "runtime-data"

KILLED_SYSCALLS_PATCH = SCRIPT_DIR / "patch_killed_syscalls.py"

# A bottle's hnp programs install as the private native package wlapp 1.0, at
# /data/app/wlapp.org/wlapp_1.0/bin in the app sandbox (Index.ets HNP_BIN).
HNP_NAME = "wlapp"
HNP_VERSION = "1.0"
# hvigor's signer cannot sign the executables inside a native package; this
# release of OpenHarmony's Java signer can (Apache-2.0).
SIGNER_URL = ("https://raw.githubusercontent.com/openharmony/developtools_hapsigner/"
              "24ef0c843d94f68d16975cac42e377f31def48c6/dist/hap-sign-tool.jar")
SIGNER_SHA256 = "9cee965bb292080a0d29fb48450091f5dcc9d010bb18da39175f581ffed7c4f4"
SIGNER = STORE / "tools/hap-sign-tool.jar"


def log(message):
    print(f"ohos-wl: {message}", file=sys.stderr)


def die(message):
    log(message)
    sys.exit(1)


def run(cmd, **kwargs):
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def output(cmd):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True).stdout


def hdc(*args):
    return output([HDC, "-t", DEVICE, *args])


def tool_env():
    env = dict(os.environ)
    env["TMPDIR"] = env.get("TMPDIR", "/data/storage/el2/base/cache")
    for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env.pop(key, None)
    env["TOOL_HOME"] = str(TOOL_HOME)
    env["OHOS_SDK"] = str(TOOL_HOME / "sdk/default/openharmony")
    return env


# -- ELF helpers ------------------------------------------------------------

def dynamic(path, tag):
    text = output(["readelf", "-d", path])
    return [line.split("[", 1)[1].rstrip("]") for line in text.splitlines() if f"({tag})" in line]


def needed(path):
    return dynamic(path, "NEEDED")


def on_system(name):
    return any((d / name).exists() for d in SYSTEM_DIRS)


def fix_object(path, foreign):
    """Prepares a copied object for the app sandbox.

    Our own builds (the native output and the Mesa, GTK, and Ghostty pool)
    are left untouched: rewriting libentry.so with patchelf and strip
    stopped its NAPI module from registering, and rewritten Mesa objects
    made every GL client exit at start. Foreign objects get an $ORIGIN runpath, their EGL, Vulkan,
    and libc names mapped, and seccomp-killed syscalls patched.
    """
    if not foreign:
        return
    run(["patchelf", "--set-rpath", "$ORIGIN", path])
    # Stripping only saves space; a few objects have tables strip rejects.
    subprocess.run(["strip", "--strip-unneeded", str(path)], stderr=subprocess.DEVNULL)
    deps = needed(path)
    # Mesa's EGL ships as libEGL.so.1; plain libEGL.so is the system driver.
    if "libEGL.so" in deps:
        run(["patchelf", "--replace-needed", "libEGL.so", "libEGL.so.1", path])
    # One Vulkan loader per process: the system's, which knows the GPU driver.
    if "libvulkan.so.1" in deps:
        run(["patchelf", "--replace-needed", "libvulkan.so.1", "libvulkan.so", path])
    # Alpine's musl libc is the HarmonyOS libc.
    if "libc.musl-aarch64.so.1" in deps:
        run(["patchelf", "--replace-needed", "libc.musl-aarch64.so.1", "libc.so", path])
    # The sandbox kills some newer syscalls; inline ones are made to fail.
    run([sys.executable, KILLED_SYSCALLS_PATCH, path], stdout=subprocess.DEVNULL)


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() or target.is_symlink():
        target.unlink()
    # The FUSE storage layer refuses cp of binaries; copy the bytes.
    with open(source, "rb") as src, open(target, "wb") as dst:
        shutil.copyfileobj(src, dst)


# Sonames fix_object maps to, or that only the system can provide: never bottled.
PLATFORM_SONAMES = {"libEGL.so", "libc.musl-aarch64.so.1", "libc.so", "libohinput.so", "libvulkan.so.1", "libvulkan.so"}


def collect(roots, search_dirs, target_dir, provided=(), fetch=None, system=True, origins=None):
    """Copies roots and their NEEDED closure into target_dir by soname.

    roots maps installed names to source paths. Libraries in provided, or on
    the system when system is true, are left out. A library in no search
    directory comes from fetch(name) when given. Returns the names copied.
    """
    target_dir.mkdir(parents=True, exist_ok=True)
    queue = list(roots.items())
    done = set(provided)
    copied = []
    while queue:
        name, source = queue.pop()
        if name in done:
            continue
        done.add(name)
        if source is None:
            source = next((d / name for d in search_dirs if (d / name).is_file()), None)
            if source is None and fetch:
                source = fetch(name)
            if source is None:
                die(f"cannot find {name}")
        target = target_dir / name
        if origins is not None:
            origins[name] = origin(source)
        copy_file(source, target)
        fix_object(target, foreign=Path(source).parent not in (NATIVE_OUT, POOL))
        copied.append(name)
        for dep in needed(target):
            if dep in done or dep in PLATFORM_SONAMES:
                continue
            if system and on_system(dep) and dep not in RUNTIME_ROOTS:
                continue
            queue.append((dep, None))
    return copied


def library_dirs():
    dirs = [NATIVE_OUT, POOL, BREW / "lib"]
    dirs += sorted((BREW / "opt").glob("*/lib"))
    return dirs


# -- Alpine -----------------------------------------------------------------

def alpine_dir():
    """The cache for the current Alpine branch (~/.ohos-wl/alpine/<branch>)."""
    return STORE / "alpine" / ALPINE_BRANCH[0]


def use_alpine_branch(spec):
    ALPINE_BRANCH[0] = spec.get("alpineBranch", ALPINE_STABLE)


@functools.cache
def alpine_index(branch):
    entries = []
    # testing exists only on edge.
    for repo in ("main", "community", "testing") if branch == "edge" else ("main", "community"):
        index = STORE / "alpine" / branch / f"APKINDEX-{repo}"
        if not index.exists():
            index.parent.mkdir(parents=True, exist_ok=True)
            url = f"{ALPINE_MIRROR}/{branch}/{repo}/aarch64/APKINDEX.tar.gz"
            with urllib.request.urlopen(url, timeout=180) as response, open(index.with_suffix(".tgz"), "wb") as out:
                shutil.copyfileobj(response, out)
            with tarfile.open(index.with_suffix(".tgz")) as archive:
                index.write_bytes(archive.extractfile("APKINDEX").read())
        record = {"repo": repo}
        for line in index.read_text().splitlines():
            if not line:
                entries.append(record)
                record = {"repo": repo}
            elif line[1:2] == ":":
                record[line[0]] = line[2:]
    return entries


def alpine_fetch(want):
    """Unpacks the package named want, or providing so:want, under alpine_dir()/root."""
    for entry in alpine_index(ALPINE_BRANCH[0]):
        provides = " " + entry.get("p", "") + " "
        if entry.get("P") == want or f" {want}=" in provides or f" {want} " in provides:
            marker = alpine_dir() / "root" / f".fetched-{entry['P']}"
            if not marker.exists():
                url = f"{ALPINE_MIRROR}/{ALPINE_BRANCH[0]}/{entry['repo']}/aarch64/{entry['P']}-{entry['V']}.apk"
                log(f"fetching Alpine {entry['P']} {entry['V']}")
                apk = alpine_dir() / f"{entry['P']}.apk"
                with urllib.request.urlopen(url, timeout=600) as response, open(apk, "wb") as out:
                    shutil.copyfileobj(response, out)
                (alpine_dir() / "root").mkdir(parents=True, exist_ok=True)
                with tarfile.open(apk, "r:gz") as archive:
                    archive.extractall(alpine_dir() / "root", filter="tar")
                    files = [m.name for m in archive.getmembers() if m.isfile() and not m.name.startswith(".")]
                # The marker records what the package installed, for sources.json.
                marker.write_text(json.dumps({"package": entry["P"], "version": entry["V"],
                                              "license": entry.get("L", ""), "files": files}))
            return alpine_dir() / "root"
    die(f"no Alpine package provides {want}")


@functools.cache
def alpine_owners(branch):
    """Maps each installed path under the branch's root to its package's
    name, version, and licence, from the fetch markers. Markers written
    before they carried file lists are filled from the cached .apk."""
    index = {entry["P"]: entry for entry in alpine_index(branch)}
    root = STORE / "alpine" / branch / "root"
    owners = {}
    for marker in root.glob(".fetched-*"):
        try:
            record = json.loads(marker.read_text())
        except ValueError:
            package = marker.name[len(".fetched-"):]
            apk = STORE / "alpine" / branch / f"{package}.apk"
            entry = index.get(package, {})
            files = []
            if apk.exists():
                with tarfile.open(apk, "r:gz") as archive:
                    files = [m.name for m in archive.getmembers() if m.isfile() and not m.name.startswith(".")]
            record = {"package": package, "version": entry.get("V", ""), "license": entry.get("L", ""),
                      "files": files}
            marker.write_text(json.dumps(record))
        for name in record["files"]:
            owners[name] = {key: record[key] for key in ("package", "version", "license")}
    return owners


def origin(source):
    """Where a bundled file came from, for sources.json."""
    source = Path(source)
    for branch_dir in (STORE / "alpine").glob("*"):
        root = branch_dir / "root"
        if root in source.parents:
            owners = alpine_owners(branch_dir.name)
            # A soname is often a symlink; the package lists the file it names.
            owner = owners.get(str(source.relative_to(root)))
            real = source.resolve()
            if not owner and root.resolve() in real.parents:
                owner = owners.get(str(real.relative_to(root.resolve())))
            return {"from": f"alpine {branch_dir.name}", **(owner or {})}
    real = source.resolve()
    if "Cellar" in real.parts:
        at = real.parts.index("Cellar")
        return {"from": "harmonybrew", "package": real.parts[at + 1], "version": real.parts[at + 2]}
    if real.parent == NATIVE_OUT.resolve():
        return {"from": "ohos-wayland runtime build", "license": "see REUSE.toml"}
    if real.parent == POOL.resolve():
        return {"from": "ohos-wayland library pool, built from sources.tsv"}
    if ROOT.resolve() in real.parents:
        return {"from": f"this repository: {real.relative_to(ROOT.resolve())}"}
    return {"from": source.name}


def alpine_library(name):
    for sub in ("lib", "usr/lib"):
        path = alpine_dir() / "root" / sub / name
        if path.is_file():
            return path
    alpine_fetch(f"so:{name}")
    for sub in ("lib", "usr/lib"):
        path = alpine_dir() / "root" / sub / name
        if path.is_file():
            return path
    die(f"Alpine has no {name}")


# -- runtime ----------------------------------------------------------------

def add_tree(archive, source, arcname):
    if not source.is_dir():
        die(f"missing data directory {source}")
    # Brew links data into its Cellar, and app storage refuses symlinks, so
    # archives hold real files (the archive dereferences links).
    archive.add(source.resolve(), arcname=arcname)


NINJA = TOOL_HOME / "sdk/default/openharmony/native/build-tools/cmake/bin/ninja"
NINJA_TREE = APP_ROOT / "entry/.cxx/default/default/debug/arm64-v8a"


def compile_native():
    """Compiles the native runtime with ninja in the tree the template HAP
    build configured, which is about 25 s faster than a hvigor build. Returns
    None when there is no such tree yet, else whether anything was rebuilt."""
    if not (NINJA_TREE / "build.ninja").exists():
        return None
    log("compiling the native runtime (ninja)")
    result = subprocess.run([NINJA, "-C", NINJA_TREE], env=tool_env(), capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout[-4000:])
        die("native compile failed")
    return "no work to do" not in result.stdout


def cmd_runtime(args):
    if "--no-build" not in args and ("--full" in args or compile_native() is None):
        log("compiling the native runtime (template HAP build)")
        for stale in (APP_ROOT / "entry/build/default/intermediates/cmake/default/obj/arm64-v8a").glob("libwayland-*.so.0"):
            stale.unlink()
        result = subprocess.run([DEVECOCLI, "build", "--build-mode", "debug"], cwd=APP_ROOT,
                                env=tool_env(), capture_output=True, text=True)
        if "BUILD SUCCESSFUL" not in result.stdout:
            sys.stdout.write(result.stdout[-4000:])
            die("runtime build failed")
    lib = RUNTIME / "lib"
    if lib.exists():
        shutil.rmtree(lib)
    roots = {name: None for name in RUNTIME_ROOTS}
    origins = {}
    copied = collect(roots, library_dirs(), lib, origins=origins)
    (RUNTIME / "sources.json").write_text(json.dumps({"lib": origins}, indent=2, sort_keys=True) + "\n")
    log(f"runtime libraries: {len(copied)}")
    for package in RUNTIME_ALPINE:
        alpine_fetch(package)
    data = RUNTIME / "runtime.tar"
    # Written beside the old archive and swapped in only when complete, so a
    # missing data tree leaves the previous runtime.tar intact.
    partial = data.with_suffix(".tar.partial")
    with tarfile.open(partial, "w", format=tarfile.GNU_FORMAT, dereference=True) as archive:
        for source, relative in RUNTIME_DATA:
            add_tree(archive, source, f"runtime/{relative}")
        for package, trees in RUNTIME_ALPINE.items():
            for source, relative in trees:
                add_tree(archive, alpine_dir() / "root" / source, f"runtime/{relative}")
        archive.add(RUNTIME_STYLE / "fonts.conf", arcname="runtime/fonts.conf")
        add_tree(archive, RUNTIME_STYLE / "gtk-4.0", "runtime/etc/gtk-4.0")
        add_tree(archive, RUNTIME_STYLE / "icons", "runtime/share/icons")
    partial.replace(data)
    log(f"runtime data: {data.stat().st_size // 1048576} MiB")


# -- bottles ----------------------------------------------------------------

def load_bottle(name):
    path = APPS / name / "bottle.json"
    if not path.exists():
        die(f"no bottle definition {path}")
    return json.loads(path.read_text()), path.parent


def bottle_stale(name):
    """Whether bottle <name> is older than its definition, the files it
    names, or the runtime it was collected against."""
    stamp = BOTTLES / name / "app.json"
    if not stamp.exists():
        return True
    spec, spec_dir = load_bottle(name)
    use_alpine_branch(spec)
    # The packing rules live in this script, so a change to it re-bottles.
    inputs = [spec_dir / "bottle.json", RUNTIME / "lib", Path(__file__).resolve(), KILLED_SYSCALLS_PATCH]
    for field in ("programs", "hnp"):
        inputs += program_paths(name, spec, spec_dir, field).values() if all(
            not source.startswith("alpine:") or (alpine_dir() / "root" / source[7:]).exists()
            for source in spec.get(field, {}).values()) else [Path("/nonexistent")]
    built = stamp.stat().st_mtime
    return any(not path.exists() or path.stat().st_mtime > built for path in inputs)


def cmd_bottle(args):
    if len(args) != 1:
        die("usage: bottle <name>")
    name = args[0]
    if not (RUNTIME / "lib").is_dir():
        die("run `runtime` first")
    spec, spec_dir = load_bottle(name)
    use_alpine_branch(spec)
    bottle = BOTTLES / name
    if bottle.exists():
        shutil.rmtree(bottle)
    (bottle / "lib").mkdir(parents=True)
    for package in spec.get("alpine", []):
        alpine_fetch(package)
    roots = program_paths(name, spec, spec_dir, "programs")
    runtime_libs = {p.name for p in (RUNTIME / "lib").iterdir()}
    search = library_dirs()
    if any(source.startswith("alpine:") for source in spec.get("programs", {}).values()):
        search = [alpine_dir() / "root/lib", alpine_dir() / "root/usr/lib"] + search
    sources = {"lib": {}, "hnp": {}}
    copied = collect(roots, search, bottle / "lib", provided=runtime_libs, origins=sources["lib"])
    if "libgtk-4.so.1" in copied:
        # GTK apps get native HarmonyOS title bars (gtk_titlebar_bridge.cpp).
        copied += collect({TITLEBAR_BRIDGE: NATIVE_OUT / TITLEBAR_BRIDGE}, search, bottle / "lib",
                          provided=runtime_libs | set(copied))
    log(f"{name}: {len(copied)} objects")
    hnp_roots = program_paths(name, spec, spec_dir, "hnp")
    if hnp_roots:
        # Stock programs get Alpine's libraries throughout, even where the
        # system has the soname: older system, brew, and pool builds of the
        # same names lack symbols they need (node and the system libcrypto).
        # Only the runtime's roots are shared: the graphics stack and shims. The runtime
        # (Mesa, Wayland, the exec shims) still comes from the HAP libs.
        alpine = [alpine_dir() / "root/lib", alpine_dir() / "root/usr/lib"]
        hnp = collect(hnp_roots, alpine, bottle / "hnp/bin", provided=set(RUNTIME_ROOTS), fetch=alpine_library,
                      system=False, origins=sources["hnp"])
        log(f"{name}: {len(hnp)} objects in the native package")
        if "libgtk-4.so.1" in hnp:
            # The title bar bridge binds to the program's own GTK, so it
            # ships alone (exec_preload.cpp loads it).
            copy_file(NATIVE_OUT / TITLEBAR_BRIDGE, bottle / "lib" / TITLEBAR_BRIDGE)
    manifest = dict(spec["manifest"])
    manifest["name"] = name
    data = spec.get("data", {})
    if data:
        archive_path = bottle / "data.tar"
        with tarfile.open(archive_path, "w", format=tarfile.GNU_FORMAT, dereference=True) as archive:
            for relative, source in data.items():
                source_path = Path(source[len("alpine:"):]) if source.startswith("alpine:") else None
                tree = alpine_dir() / "root" / source_path if source_path else (spec_dir / source).resolve()
                add_tree(archive, tree, relative)
        manifest["data"] = ["data.tar"]
    (bottle / "app.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Every bundled object's origin and licence, for builders and for the
    # LGPL and GPL source obligations of what a HAP carries.
    (bottle / "sources.json").write_text(json.dumps(sources, indent=2, sort_keys=True) + "\n")
    icon = None
    if spec.get("icon"):
        source = spec["icon"]
        path = alpine_dir() / "root" / source[len("alpine:"):] if source.startswith("alpine:") else (spec_dir / source).resolve()
        if not path.is_file():
            die(f"{name}: missing icon {path}")
        icon = "icon" + path.suffix.lower()
        copy_file(path, bottle / icon)
    meta = {"label": spec.get("label", name), "icon": icon,
            "iconBackground": spec.get("iconBackground"),
            "background": spec.get("background"),
            "permissions": spec.get("permissions", [])}
    (bottle / "bottle-meta.json").write_text(json.dumps(meta) + "\n")


def program_paths(name, spec, spec_dir, field):
    """Maps a bottle field's installed names to source files."""
    roots = {}
    for installed, source in spec.get(field, {}).items():
        if source.startswith("alpine:"):
            path = alpine_dir() / "root" / source[len("alpine:"):]
        elif source.startswith("native:"):
            path = NATIVE_OUT / source[len("native:"):]
        elif source.startswith("pool:"):
            path = POOL / source[len("pool:"):]
        else:
            path = (spec_dir / source).resolve()
        if not path.is_file():
            die(f"{name}: missing program {path}")
        roots[installed] = path
    return roots


# -- HAPs -------------------------------------------------------------------

TEMPLATE_FILES = ["AppScope", "build-profile.json5", "code-linter.json5", "hvigor", "hvigorfile.ts",
                  "oh-package.json5", "oh-package-lock.json5", "oh_modules"]
TEMPLATE_ENTRY = ["build-profile.json5", "hvigorfile.ts", "obfuscation-rules.txt", "oh-package.json5",
                  "oh-package-lock.json5", "oh_modules", "src/main/ets", "src/main/module.json5",
                  "src/main/resources/base", "src/main/cpp/types"]


@functools.cache
def bundle_prefix():
    """The builder's bundle name prefix, such as com.example. Bundle names
    are tied to the AppGallery Connect account that signs them, so each
    builder chooses their own, once: OHOS_WL_BUNDLE_PREFIX, else the saved
    ~/.ohos-wl/bundle-prefix, else a prompt that saves it."""
    import re
    prefix = os.environ.get("OHOS_WL_BUNDLE_PREFIX")
    if not prefix and BUNDLE_PREFIX_FILE.exists():
        prefix = BUNDLE_PREFIX_FILE.read_text().strip()
    if not prefix:
        if not sys.stdin.isatty():
            die(f"no bundle prefix; set OHOS_WL_BUNDLE_PREFIX or write one to {BUNDLE_PREFIX_FILE}, e.g. com.yourname.")
        prefix = input("Bundle name prefix for your apps (reverse domain, e.g. com.yourname.): ").strip()
        if prefix and not prefix.endswith("."):
            prefix += "."
        STORE.mkdir(parents=True, exist_ok=True)
        BUNDLE_PREFIX_FILE.write_text(prefix + "\n")
    if not re.fullmatch(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\.", prefix):
        die(f"bad bundle prefix {prefix!r}: use lowercase reverse-domain form ending in a dot, e.g. com.yourname.")
    return prefix


def bundle_name(name):
    # OHOS_WL_BUNDLE reuses an existing bundle (and its signing profile).
    return os.environ.get("OHOS_WL_BUNDLE") or bundle_prefix() + name.replace("-", "_")


TEMPLATE_BUNDLE = "com.example.myapplication"


def project_dir(name):
    """build/haps/<name>; a bundle override builds elsewhere, so it never
    replaces the project DevEco Studio signs for the app's own bundle."""
    override = os.environ.get("OHOS_WL_BUNDLE")
    return HAPS / (f"{name}-{override}" if override else name)


def copy_tree(source, target):
    if source.is_dir():
        shutil.copytree(source, target, symlinks=True, dirs_exist_ok=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def edit_json5(path, old, new):
    text = path.read_text()
    if old not in text:
        die(f"{path}: expected {old!r}")
    path.write_text(text.replace(old, new, 1))


def profile_bundle(config):
    """The bundle name a signing config's profile is bound to."""
    try:
        data = Path(config["material"]["profile"]).read_bytes()
    except (KeyError, OSError):
        return None
    marker = b'"bundle-name":"'
    start = data.find(marker)
    return data[start + len(marker):data.index(b'"', start + len(marker))].decode() if start >= 0 else None


def harvest_signing(project, bundle):
    """Caches a signing config DevEco Studio wrote into an earlier project.

    DevEco Studio's automatic signing (File > Project Structure > Signing
    Configs) writes the profile into ~/.ohos/config and the config into the
    project's build-profile.json5; later projects for the bundle reuse it.
    """
    profile = project / "build-profile.json5"
    if not profile.exists():
        return
    try:
        configs = read_signing(profile.read_text())
    except (ValueError, json.JSONDecodeError):
        return
    if configs and profile_bundle(configs[0]) == bundle:
        SIGNING.mkdir(parents=True, exist_ok=True)
        cached = SIGNING / f"{bundle}.json"
        entry = configs[0]
        if cached.exists():
            previous = json.loads(cached.read_text())
            if previous.get("material") == entry.get("material") and "permissions" in previous:
                entry["permissions"] = previous["permissions"]
        cached.write_text(json.dumps(entry, indent=2) + "\n")
        log(f"cached the signing profile for {bundle}")


def read_signing(text):
    start = text.index('"signingConfigs"')
    open_bracket = text.index("[", start)
    depth = 0
    for index in range(open_bracket, len(text)):
        depth += {"[": 1, "]": -1}.get(text[index], 0)
        if depth == 0:
            return json.loads(text[open_bracket:index + 1])
    return []


def declared_permissions(project):
    text = (project / "entry/src/main/module.json5").read_text()
    return sorted(set(re.findall(r'"name": "(ohos\.permission\.[A-Za-z0-9_.]+)"', text)))


def profile_supports_permissions(config, permissions):
    """Checks the profile's ACLs so a cache without metadata can still be reused."""
    try:
        data = Path(config["material"]["profile"]).read_bytes()
        marker = b'"allowed-acls":'
        start = data.index(marker) + len(marker)
        allowed = set(json.loads(data[start:data.index(b']', start) + 1]))
    except (KeyError, OSError, ValueError):
        return False
    required = set(permissions) - {"ohos.permission.INTERNET"}
    return required.issubset(allowed) and allowed.issubset(permissions)


def signing_config(project, bundle):
    """Returns the signingConfigs entry for bundle, generating it once per
    permission set: a debug profile grants only what the module declared
    when it was generated."""
    cached = SIGNING / f"{bundle}.json"
    permissions = declared_permissions(project)
    if cached.exists():
        config = json.loads(cached.read_text())
        if config.pop("permissions", None) == permissions or profile_supports_permissions(config, permissions):
            return config
    if bundle == TEMPLATE_BUNDLE:
        return template_signing()
    # Oniro signs through AppGallery Connect with any region's account;
    # devecocli accepts only CN accounts.
    log(f"generating a debug signing profile for {bundle} (oniro-app)")
    env = dict(tool_env(), ONIRO_HARMONYOS_SDK_PATH=str(oniro_sdk()))
    result = subprocess.run([ONIRO, "sign", "--harmonyos", "--product", "default", project], env=env,
                            capture_output=True, text=True)
    sys.stderr.write(result.stdout[-2000:] + result.stderr[-2000:])
    configs = []
    if result.returncode == 0:
        configs = read_signing((project / "build-profile.json5").read_text())
    if not configs or profile_bundle(configs[0]) != bundle:
        die(f"oniro-app sign failed for {bundle}; run `oniro-app auth login` on the host first")
    SIGNING.mkdir(parents=True, exist_ok=True)
    cached.write_text(json.dumps(dict(configs[0], permissions=permissions), indent=2) + "\n")
    return configs[0]


def oniro_sdk():
    """The HarmonyOS SDK for oniro-app: links to the command-line tools' SDK
    plus hap-sign-tool.jar, which those tools do not ship and Oniro needs."""
    real = TOOL_HOME / "sdk"
    shadow = STORE / "sdk"
    lib = shadow / "default/openharmony/toolchains/lib"
    if (lib / "hap-sign-tool.jar").exists():
        return shadow
    if shadow.exists():
        shutil.rmtree(shadow)
    # Link every entry along the path to toolchains/lib, then lib's files.
    path = Path()
    for part in ("default", "openharmony", "toolchains", "lib"):
        (shadow / path / part).mkdir(parents=True)
        for entry in (real / path).iterdir():
            if entry.name != part and not (shadow / path / entry.name).exists():
                (shadow / path / entry.name).symlink_to(entry)
        path /= part
    for entry in (real / path).iterdir():
        if entry.name != "hap-sign-tool.jar":
            (lib / entry.name).symlink_to(entry)
    (lib / "hap-sign-tool.jar").symlink_to(signer())
    return shadow


def template_signing():
    """The template bundle's signing config, cached like every other bundle's.
    The repository's build-profile.json5 carries none, because a config names
    local key files and encrypted passwords."""
    cached = SIGNING / f"{TEMPLATE_BUNDLE}.json"
    if not cached.exists():
        die(f"no signing profile for {TEMPLATE_BUNDLE}. Run `oniro-app sign --harmonyos` in app/, then "
            f"`ohos_wl.py project <name>` once with OHOS_WL_BUNDLE={TEMPLATE_BUNDLE} to cache it.")
    return json.loads(cached.read_text())


def write_signing(project, config):
    path = project / "build-profile.json5"
    text = path.read_text()
    start = text.index('"signingConfigs"')
    open_bracket = text.index("[", start)
    depth = 0
    for index in range(open_bracket, len(text)):
        depth += {"[": 1, "]": -1}.get(text[index], 0)
        if depth == 0:
            text = text[:open_bracket] + json.dumps([config], indent=2) + text[index + 1:]
            break
    if path.read_text() != text:
        path.write_text(text)


def cmd_project(args):
    if len(args) != 1:
        die("usage: project <name>")
    write_project(args[0])
    print(project_dir(args[0]))


def write_project(name):
    bottle = BOTTLES / name
    if not (bottle / "app.json").exists():
        die(f"run `bottle {name}` first")
    meta = json.loads((bottle / "bottle-meta.json").read_text())
    bundle = bundle_name(name)
    target = project_dir(name)
    harvest_signing(target, bundle)
    # Assemble afresh, then copy only changed files into the kept project so
    # hvigor finds its earlier outputs up to date.
    project = HAPS / ".stage" / target.name
    if project.exists():
        shutil.rmtree(project)
    for item in TEMPLATE_FILES:
        copy_tree(APP_ROOT / item, project / item)
    for item in TEMPLATE_ENTRY:
        copy_tree(APP_ROOT / "entry" / item, project / "entry" / item)
    edit_json5(project / "AppScope/app.json5", f'"bundleName": "{TEMPLATE_BUNDLE}"',
               f'"bundleName": "{bundle}"')
    for strings in (project / "AppScope/resources/base/element/string.json",
                    project / "entry/src/main/resources/base/element/string.json"):
        data = json.loads(strings.read_text())
        for item in data.get("string", []):
            if item["name"] in ("app_name", "EntryAbility_label"):
                item["value"] = meta["label"]
        strings.write_text(json.dumps(data, indent=2) + "\n")
    # The start window shows the app's colour, so launch, wait, and first
    # frame read as one surface.
    if meta.get("background"):
        colors = project / "entry/src/main/resources/base/element/color.json"
        data = json.loads(colors.read_text())
        for item in data["color"]:
            if item["name"] == "start_window_background":
                item["value"] = meta["background"]
        colors.write_text(json.dumps(data, indent=2) + "\n")
    write_icons(project, bottle, meta)
    # The native code is prebuilt by `runtime`; the HAP only packages it.
    profile = project / "entry/build-profile.json5"
    text = profile.read_text()
    start = text.index('"externalNativeOptions"')
    end = text.index("}", start) + 1
    profile.write_text(text[:start] + text[end:])
    libs = project / "entry/libs/arm64-v8a"
    for source in list((RUNTIME / "lib").iterdir()) + list((bottle / "lib").iterdir()):
        copy_file(source, libs / source.name)
    raw = project / "entry/src/main/resources/rawfile"
    raw.mkdir(parents=True, exist_ok=True)
    copy_file(RUNTIME / "runtime.tar", raw / "runtime.tar")
    copy_file(bottle / "app.json", raw / "app.json")
    if (bottle / "sources.json").exists():
        copy_file(bottle / "sources.json", raw / "sources.json")
    if (RUNTIME / "sources.json").exists():
        copy_file(RUNTIME / "sources.json", raw / "runtime-sources.json")
    if (bottle / "data.tar").exists():
        copy_file(bottle / "data.tar", raw / "data.tar")
    permissions = json.loads((bottle / "bottle-meta.json").read_text()).get("permissions", [])
    if permissions and bundle == TEMPLATE_BUNDLE:
        # The borrowed template profile carries no ACLs; installing would fail.
        log(f"leaving out {', '.join(permissions)}: the template profile cannot grant them")
        permissions = []
    if bundle == TEMPLATE_BUNDLE:
        module = project / "entry/src/main/module.json5"
        data = json.loads(re.sub(r",\s*([\]}])", r"\1", module.read_text()))
        data["module"]["requestPermissions"] = [
            entry for entry in data["module"]["requestPermissions"] if entry["name"] == "ohos.permission.INTERNET"
        ]
        module.write_text(json.dumps(data, indent=2) + "\n")
    if permissions:
        module = project / "entry/src/main/module.json5"
        entries = "".join(f',\n      {{\n        "name": "{p}"\n      }}' for p in permissions)
        edit_json5(module, '"name": "ohos.permission.INTERNET"\n      }', '"name": "ohos.permission.INTERNET"\n      }' + entries)
    if (bottle / "hnp/bin").is_dir():
        edit_json5(project / "entry/src/main/module.json5", '"compressNativeLibs": false,',
                   '"compressNativeLibs": false,\n    "hnpPackages": [\n'
                   f'      {{ "package": "{HNP_NAME}.hnp", "type": "private" }}\n    ],')
    cached = SIGNING / f"{bundle}.json"
    if cached.exists() or bundle == TEMPLATE_BUNDLE:
        write_signing(project, signing_config(project, bundle))
    sync_tree(project, target)
    shutil.rmtree(project)
    return target


# Build state hvigor keeps in a project; never part of the assembled tree.
PROJECT_STATE = {".hvigor", "build", "entry/build", "entry/.cxx", "build.log", "oh_modules", "entry/oh_modules"}


def sync_tree(source, target):
    """Makes target hold source's files, rewriting only those whose bytes
    differ and keeping hvigor's build state."""
    import filecmp
    wanted = set()
    for path in source.rglob("*"):
        relative = path.relative_to(source)
        wanted.add(relative)
        destination = target / relative
        if path.is_dir():
            destination.mkdir(parents=True, exist_ok=True)
        elif not (destination.is_file() and destination.stat().st_size == path.stat().st_size
                  and filecmp.cmp(path, destination, shallow=False)):
            copy_file(path, destination)
    if not target.exists():
        return
    for path in sorted(target.rglob("*"), reverse=True):
        relative = path.relative_to(target)
        if any(relative.as_posix() == kept or relative.as_posix().startswith(kept + "/") for kept in PROJECT_STATE):
            continue
        if relative not in wanted:
            if path.is_dir() and not path.is_symlink():
                if not any(path.iterdir()):
                    path.rmdir()
            else:
                path.unlink()


RSVG = BREW / "bin/rsvg-convert"
ICON_SIZE = 1024
# The share of the layered icon's foreground the app's own icon fills; the
# launcher masks the rest to its icon shape.
ICON_SCALE = 0.72


def write_icons(project, bottle, meta):
    """Renders the app's icon as the HAP's layered icon (foreground and
    background, 1024 px) and start window icon. An app without an icon gets
    its label's first letter. Needs rsvg-convert (brew install librsvg)."""
    import base64
    if not RSVG.exists():
        log("no rsvg-convert; the HAP keeps the template icon (brew install librsvg)")
        return
    background = meta.get("iconBackground") or meta.get("background") or "#3a6ea5"
    size = ICON_SIZE
    inset = round(size * (1 - ICON_SCALE) / 2)
    inner = size - 2 * inset
    if meta.get("icon"):
        icon = bottle / meta["icon"]
        kind = "image/svg+xml" if icon.suffix == ".svg" else "image/png"
        data = base64.b64encode(icon.read_bytes()).decode()
        mark = (f'<image x="{inset}" y="{inset}" width="{inner}" height="{inner}" '
                f'href="data:{kind};base64,{data}" preserveAspectRatio="xMidYMid meet"/>')
    else:
        letter = (meta.get("label") or "?")[:1].upper()
        mark = (f'<text x="{size // 2}" y="{size // 2}" font-family="HarmonyOS Sans, sans-serif" '
                f'font-size="{inner * 0.6:.0f}" font-weight="600" fill="#ffffff" text-anchor="middle" '
                f'dominant-baseline="central">{letter}</text>')
    svg = '<svg xmlns="http://www.w3.org/2000/svg" width="{0}" height="{0}">{1}</svg>'
    layers = {
        "foreground.png": svg.format(size, mark),
        "background.png": svg.format(size, f'<rect width="{size}" height="{size}" fill="{background}"/>'),
        # The start window shows the foreground alone, on the app's colour.
        "startIcon.png": svg.format(size, mark),
    }
    for media in (project / "AppScope/resources/base/media", project / "entry/src/main/resources/base/media"):
        for name, text in layers.items():
            if name == "startIcon.png" and "AppScope" in str(media):
                continue
            source = bottle / f".{name}.svg"
            source.write_text(text)
            subprocess.run([RSVG, "-w", str(size), "-h", str(size), "-o", media / name, source], check=True)
            source.unlink()


def cmd_hap(args):
    if len(args) != 1:
        die("usage: hap <name>")
    name = args[0]
    bundle = bundle_name(name)
    project = write_project(name)
    write_signing(project, signing_config(project, bundle))
    log(f"building {bundle}")
    result = subprocess.run([DEVECOCLI, "build", "--build-mode", "debug"], cwd=project, env=tool_env(),
                            capture_output=True, text=True)
    (project / "build.log").write_text(result.stdout + result.stderr)
    if "BUILD SUCCESSFUL" not in result.stdout:
        die(f"HAP build failed; see {project / 'build.log'}")
    hnp = BOTTLES / name / "hnp"
    if hnp.is_dir():
        sign_with_hnp(project, hnp, signing_config(project, bundle))
    print(hap_path(name))


def pack_hnp(tree, out):
    """Writes tree as a native package the way OpenHarmony's hnpcli pack does
    (startup_appspawn service/hnp/pack): entries under <name>/ with their file
    modes, plus <name>/hnp.json."""
    import stat
    import zipfile
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(tree.rglob("*")):
            arcname = f"{HNP_NAME}/{path.relative_to(tree)}"
            info = zipfile.ZipInfo(arcname + ("/" if path.is_dir() else ""))
            info.external_attr = ((stat.S_IFDIR if path.is_dir() else stat.S_IFREG) | 0o755) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, b"" if path.is_dir() else path.read_bytes())
        config = {"type": "hnp-config", "name": HNP_NAME, "version": HNP_VERSION, "install": {"links": []}}
        info = zipfile.ZipInfo(f"{HNP_NAME}/hnp.json")
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, json.dumps(config, indent=4).encode() + b"\0")


def signer():
    import hashlib
    if not SIGNER.exists() or hashlib.sha256(SIGNER.read_bytes()).hexdigest() != SIGNER_SHA256:
        SIGNER.parent.mkdir(parents=True, exist_ok=True)
        log("fetching OpenHarmony hap-sign-tool.jar")
        with urllib.request.urlopen(SIGNER_URL, timeout=600) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SIGNER_SHA256:
            die("hap-sign-tool.jar does not match its pinned hash")
        SIGNER.write_bytes(data)
    return SIGNER


def sign_with_hnp(project, hnp, config):
    """Adds the native package to the unsigned HAP and signs it, replacing
    the HAP hvigor signed. hvigor does not package .hnp files, so it goes in
    afterwards, stored, as Qt Creator's HarmonyOS packaging does."""
    import zipfile
    outputs = project / "entry/build/default/outputs/default"
    package = outputs / f"{HNP_NAME}.hnp"
    pack_hnp(hnp, package)
    unsigned = outputs / "entry-default-unsigned-hnp.hap"
    copy_file(outputs / "entry-default-unsigned.hap", unsigned)
    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_STORED) as archive:
        archive.write(package, f"hnp/arm64-v8a/{HNP_NAME}.hnp")
    material = config["material"]
    # The material directory sits beside the keystore (store_signing).
    secrets = Path(material["storeFile"]).parent
    password = lambda field: output(["node", SCRIPT_DIR / "deveco_password.js", secrets, material[field]])
    log(f"signing with {HNP_NAME}.hnp ({package.stat().st_size // 1048576} MiB)")
    result = subprocess.run(
        ["java", "-jar", str(signer()), "sign-app", "-mode", "localSign", "-signAlg", material.get("signAlg", "SHA256withECDSA"),
         "-signCode", "1", "-keyAlias", material["keyAlias"], "-keyPwd", password("keyPassword"),
         "-keystorePwd", password("storePassword"), "-keystoreFile", material["storeFile"],
         "-appCertFile", material["certpath"], "-profileFile", material["profile"],
         "-inFile", str(unsigned), "-outFile", str(hap_path_in(project))],
        env=tool_env(), capture_output=True, text=True)
    unsigned.unlink()
    if result.returncode != 0 or "success" not in (result.stdout + result.stderr).lower():
        sys.stderr.write("\n".join(line for line in (result.stdout + result.stderr).splitlines()
                                    if "pwd" not in line.lower() and "password" not in line.lower())[-3000:])
        die("hap-sign-tool failed")


def hap_path_in(project):
    return project / "entry/build/default/outputs/default/entry-default-signed.hap"


def hap_path(name):
    return hap_path_in(project_dir(name))


def cmd_install(args):
    if len(args) != 1:
        die("usage: install <name>")
    result = hdc("install", "-r", hap_path(args[0]))
    print(result.strip())
    if "install bundle successfully" not in result:
        die("install failed")


def cmd_run(args, quiet=False):
    if not 1 <= len(args) <= 2:
        die("usage: run <name> [test]")
    command = f"aa start -a EntryAbility -b {bundle_name(args[0])}"
    if len(args) == 2:
        command += f" --ps test {args[1]}"
    result = hdc("shell", command).strip()
    if not quiet:
        print(result)


def test_log(name, test):
    return f"/data/app/el2/100/base/{bundle_name(name)}/cache/ohos_wl_app:{name}:{test}.log"


def test_output(name, test, since):
    """The test's forwarded output: its log file and hilog lines since launch.

    A long-running program's stdout to a file stays block-buffered, while
    the direct child forwards each line to hilog as it arrives.
    """
    text = hdc("shell", f"cat {test_log(name, test)} 2>/dev/null")
    lines = hdc("shell", "hilog -x").splitlines()
    return text + "\n".join(line for line in lines if line[:14] >= since)


def cmd_test(args):
    import time
    spec, _ = load_bottle("testing")
    tests = spec["manifest"]["tests"]
    names = args or [name for name, test in tests.items() if not test.get("manual")]
    failed = []
    try:
        run_tests(tests, names, failed)
    finally:
        # A test app left running keeps the GPU and the compositor busy and
        # skews whatever runs next, so every run ends with it closed.
        cmd_stop(["testing"])
    print(f"{len(names) - len(failed)} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


def run_tests(tests, names, failed):
    import time
    for test in names:
        expect = tests[test].get("expect")
        since = hdc("shell", "date '+%m-%d %H:%M:%S'").strip()
        # A previous run's log would pass the test before this one starts.
        hdc("shell", f"rm -f {test_log('testing', test)}")
        foreground = tests[test].get("foreground") or tests[test].get("fullscreen")
        if TESTS_IN_BACKGROUND and not foreground:
            request_background("testing", f"test {test} maximized" if tests[test].get("maximize") else f"test {test}")
        else:
            cmd_run(["testing", test], quiet=True)
            if tests[test].get("fullscreen"):
                request_background("testing", "state fullscreen")
            elif tests[test].get("maximize"):
                request_background("testing", "state maximize")
        # Poll for the expected output; "seconds" is the deadline.
        deadline = time.monotonic() + tests[test].get("seconds", 12)
        ok = expect is None
        output = ""
        while not ok and time.monotonic() < deadline:
            time.sleep(1)
            output = test_output("testing", test, since)
            ok = expect in output
        # "reject" lists output that fails the test even once expect appears.
        rejected = [text for text in tests[test].get("reject", []) if text in output]
        reason = f" (expected {expect!r})" if not ok else f" (rejected {rejected[0]!r})" if rejected else ""
        ok = ok and not rejected
        if ok and tests[test].get("edges"):
            mismatch = edge_mismatch("testing", output)
            if mismatch:
                ok = False
                reason = f" ({mismatch})"
        print(f"{'PASS' if ok else 'FAIL'} {test}{reason}", flush=True)
        if not ok:
            failed.append(test)


def edge_mismatch(name, output):
    """Why the frame's inset margins do not continue the app's edge pixels, or None."""
    import tempfile
    import time
    found = re.findall(r"CONTENT_INSETS (\d+) (\d+) (\d+) (\d+)", output)
    if not found or not any(int(value) for value in found[-1]):
        return "no content insets"
    left, top, right, bottom = (int(value) for value in found[-1])
    remote = f"/data/app/el2/100/base/{bundle_name(name)}/cache/ohos_wl_capture.ppm"
    hdc("shell", f"rm -f {remote}")
    request_background(name, "capture")
    for _ in range(20):
        time.sleep(0.5)
        if "ohos_wl_capture.ppm" in hdc("shell", f"ls {remote} 2>/dev/null"):
            break
    else:
        return "no frame captured"
    time.sleep(0.5)
    with tempfile.TemporaryDirectory() as folder:
        local = Path(folder) / "capture.ppm"
        hdc("file", "recv", remote, local)
        parts = local.read_bytes().split(b"\n", 3)
    width, height = (int(value) for value in parts[1].split())
    data = parts[3]

    def pixel(x, y):
        offset = (y * width + x) * 3
        return data[offset:offset + 3]

    def compare(margin, edge, label):
        if any(abs(a - b) > 2 for a, b in zip(pixel(*margin), pixel(*edge))):
            return f"{label} margin {margin} is {pixel(*margin).hex()}, its edge {edge} is {pixel(*edge).hex()}"
        return None

    right_edge, bottom_edge = width - right - 1, height - bottom - 1
    for y in range(top, bottom_edge + 1, 41):
        for mismatch in (compare((0, y), (left, y), "left"), compare((width - 1, y), (right_edge, y), "right")):
            if mismatch:
                return mismatch
    for x in range(left, right_edge + 1, 41):
        for mismatch in (compare((x, 0), (x, top), "top"), compare((x, height - 1), (x, bottom_edge), "bottom")):
            if mismatch:
                return mismatch
    return None


# Tests run in an app started with `aa pre-start`, whose window stays hidden,
# so a run never takes the user's focus. OHOS_WL_TEST_FOREGROUND=1 starts it
# with `aa start` instead, to watch.
TESTS_IN_BACKGROUND = not os.environ.get("OHOS_WL_TEST_FOREGROUND")


def request_background(name, request):
    """Hands a request (`test <name>` or `capture`) to the app through the
    file Index.ets polls, pre-starting the app hidden first if needed."""
    import tempfile
    import time
    bundle = bundle_name(name)
    if not hdc("shell", f"pidof {bundle}").strip():
        result = hdc("shell", f"aa pre-start -m 1 -b {bundle} -u 100").strip()
        if "successfully" not in result:
            die(f"aa pre-start failed: {result}")
        time.sleep(2)
    with tempfile.NamedTemporaryFile("w", suffix=".req", delete=False) as file:
        file.write(request + "\n")
    try:
        hdc("file", "send", "-b", bundle, file.name, "/data/storage/el2/base/cache/ohos_wl_request")
    finally:
        os.unlink(file.name)


def cmd_stop(args):
    """Closes the app: its window, its compositor, and its programs."""
    if len(args) != 1:
        die("usage: stop <name>")
    hdc("shell", f"aa force-stop {bundle_name(args[0])}")


def write_png(path, width, height, rgb):
    import struct
    import zlib

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def cmd_screenshot(args):
    """Captures the app's composed frame: only its own window's pixels.

    With --window the capture is the whole HarmonyOS window (window.snapshot),
    title bar included, still nothing outside it.
    """
    import time
    whole = "--window" in args
    args = [a for a in args if a != "--window"]
    if not 1 <= len(args) <= 2:
        die("usage: screenshot <name> [out.png] [--window]")
    name = args[0]
    if whole:
        remote = f"/data/app/el2/100/base/{bundle_name(name)}/cache/ohos_wl_window.png"
        hdc("shell", f"rm -f {remote}")
        request_background(name, "capture")
        out = Path(args[1]) if len(args) == 2 else ROOT / "build/screens" / f"{name}-window-{time.strftime('%Y%m%d-%H%M%S')}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(20):
            time.sleep(0.5)
            if "ohos_wl_window.png" in hdc("shell", f"ls {remote} 2>/dev/null"):
                break
        else:
            die("no window snapshot")
        time.sleep(0.5)
        hdc("file", "recv", remote, out)
        print(out)
        return
    remote = f"/data/app/el2/100/base/{bundle_name(name)}/cache/ohos_wl_capture.ppm"
    hdc("shell", f"rm -f {remote}")
    request_background(name, "capture")
    for _ in range(20):
        time.sleep(0.5)
        if "ohos_wl_capture.ppm" in hdc("shell", f"ls {remote} 2>/dev/null"):
            break
    else:
        die("no frame captured; is the app presenting on the direct GPU path?")
    out = Path(args[1]) if len(args) == 2 else ROOT / "build/screens" / f"{name}-{time.strftime('%Y%m%d-%H%M%S')}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    local = out.with_suffix(".ppm")
    result = hdc("file", "recv", remote, local)
    if not local.exists():
        die(f"cannot pull the capture: {result.strip()}")
    data = local.read_bytes()
    parts = data.split(b"\n", 3)
    width, height = (int(v) for v in parts[1].split())
    write_png(out, width, height, parts[3][:width * height * 3])
    local.unlink()
    print(out)


def cmd_authorize(args):
    """Lets an app with the ssh-key setup log in to the HiShell sshd."""
    if len(args) != 1:
        die("usage: authorize <name>")
    key = hdc("shell", f"cat /data/app/el2/100/base/{bundle_name(args[0])}/haps/entry/files/home/.ssh/hishell_rsa.pub").strip()
    if not key.startswith("ssh-"):
        die("no key yet; run the app once first")
    authorized = Path.home() / ".ssh/authorized_keys"
    existing = authorized.read_text() if authorized.exists() else ""
    if key in existing:
        log("key already authorized")
        return
    with open(authorized, "a") as out:
        out.write(("" if existing.endswith("\n") or not existing else "\n") + key + f" ohos-wl:{args[0]}\n")
    log(f"authorized {bundle_name(args[0])}")


# DevEco Studio's Project Structure > Signing Configs page is reachable through
# hdc uitest, but its menus are not, so signing borrows the project already
# open there: set its bundle name (and permissions), sync, generate, copy the
# materials, and finally restore the project and regenerate its own profile.
DEVECO_PROJECT_ENV = "OHOS_WL_DEVECO_PROJECT"


def deveco_generate(project, bundle, permissions):
    import time
    sys.path.insert(0, str(SCRIPT_DIR))
    import uidrive
    app = project / "AppScope/app.json5"
    text = app.read_text()
    current = text.split('"bundleName": "', 1)[1].split('"', 1)[0]
    app.write_text(text.replace(f'"bundleName": "{current}"', f'"bundleName": "{bundle}"', 1))
    module = project / "entry/src/main/module.json5"
    module_text = module.read_text()
    if permissions:
        entries = ",\n".join(f'      {{ "name": "{p}" }}' for p in permissions)
        module.write_text(module_text.replace('"deliveryWithInstall"',
                                              f'"requestPermissions": [\n{entries}\n    ],\n    "deliveryWithInstall"', 1))
    hdc("shell", "aa start -a EntryAbility -b com.huawei.devecostudio")
    # The page appears in the UI tree a few seconds after DevEco comes forward.
    for _ in range(15):
        time.sleep(2)
        if any("Generate Signing Files" in text for _k, text, _b in uidrive.controls("com.huawei.devecostudio")):
            break
    uidrive.main(["click", "Sync Now"])
    time.sleep(12)
    # The page reloads after a sync; retry until the button is back.
    for _ in range(10):
        if uidrive.main(["click", "Generate Signing Files"]) == 0:
            break
        time.sleep(2)
    else:
        die("DevEco Studio is not on Project Structure > Signing Configs")
    # Done when the profile the project points at is bound to the bundle.
    for _ in range(40):
        time.sleep(3)
        config = read_signing((project / "build-profile.json5").read_text())[0]
        if profile_bundle(config) == bundle:
            break
    else:
        die(f"DevEco Studio did not generate a profile for {bundle}")
    module.write_text(module_text)
    return config


def store_signing(bundle, config):
    """Copies a generated signing set to its own files and caches the config."""
    target = SIGNING / bundle
    target.mkdir(parents=True, exist_ok=True)
    material = dict(config["material"])
    for key, suffix in (("storeFile", ".p12"), ("certpath", ".cer"), ("profile", ".p7b")):
        destination = target / f"debug{suffix}"
        shutil.copyfile(material[key], destination)
        material[key] = str(destination)
    # hvigor decrypts the passwords with the material directory beside the keystore.
    shutil.copytree(Path(config["material"]["storeFile"]).parent / "material", target / "material",
                    dirs_exist_ok=True)
    stored = dict(config, material=material)
    (SIGNING / f"{bundle}.json").write_text(json.dumps(stored, indent=2) + "\n")
    if profile_bundle(stored) != bundle:
        die(f"generated profile is not bound to {bundle}")
    log(f"signed {bundle}")


def cmd_deveco_sign(args):
    if not args:
        die("usage: deveco-sign <name...>")
    if not os.environ.get(DEVECO_PROJECT_ENV):
        die(f"set {DEVECO_PROJECT_ENV} to a project open in DevEco Studio (or sign with oniro-app instead)")
    project = Path(os.environ[DEVECO_PROJECT_ENV])
    saved = {p: (project / p).read_text() for p in
             ("AppScope/app.json5", "entry/src/main/module.json5", "build-profile.json5")}
    original = saved["AppScope/app.json5"].split('"bundleName": "', 1)[1].split('"', 1)[0]
    try:
        for name in args:
            meta = json.loads((BOTTLES / name / "bottle-meta.json").read_text())
            bundle = bundle_name(name)
            store_signing(bundle, deveco_generate(project, bundle, meta.get("permissions", [])))
    finally:
        for path, text in saved.items():
            (project / path).write_text(text)
        # The borrowed project's own profile files were overwritten; regenerate them.
        deveco_generate(project, original, [])
        for path, text in saved.items():
            (project / path).write_text(text)
        log(f"restored {project}")


def cmd_dev(args):
    """The inner loop: every step that may be stale, then the named tests
    (or a launch, for an app that is not the testing bottle)."""
    if not args:
        die("usage: dev <name> [test...]")
    name, tests = args[0], args[1:]
    data = RUNTIME / "runtime.tar"
    data_stale = not data.exists() or any(path.stat().st_mtime > data.stat().st_mtime
                                          for path in RUNTIME_STYLE.rglob("*") if path.is_file())
    if compile_native() or not (RUNTIME / "lib").is_dir() or data_stale:
        cmd_runtime(["--no-build"])
    if bottle_stale(name):
        cmd_bottle([name])
    cmd_hap([name])
    cmd_install([name])
    if name == "testing" and not os.environ.get("OHOS_WL_BUNDLE"):
        cmd_test(tests)
    else:
        cmd_run([name], quiet=True)


def cmd_all(args):
    cmd_bottle(args)
    cmd_hap(args)
    cmd_install(args)


COMMANDS = {"runtime": cmd_runtime, "bottle": cmd_bottle, "project": cmd_project, "hap": cmd_hap, "install": cmd_install,
            "run": cmd_run, "test": cmd_test, "screenshot": cmd_screenshot,
            "authorize": cmd_authorize, "deveco-sign": cmd_deveco_sign, "all": cmd_all, "dev": cmd_dev, "stop": cmd_stop}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
