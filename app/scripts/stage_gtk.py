#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import sys

app_root = Path(__file__).resolve().parent.parent
root = app_root.parent
out = app_root / "entry/libs/arm64-v8a"
build = app_root / "entry/build/default/intermediates/cmake/default/obj/arm64-v8a"
gtk = app_root / "native-build/gtk-install"
gpu = app_root / "native-build/native_arm64-v8a/gpu-install"
brew = Path(os.environ.get("BREW_PREFIX", str(Path.home() / ".harmonybrew")))
tool_home = Path(os.environ.get("TOOL_HOME", "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0"))
readelf = tool_home / "sdk/default/openharmony/native/llvm/bin/llvm-readelf"
patchelf = os.environ.get("PATCHELF_BIN", "patchelf")
search = [gpu / "lib", Path("/usr/lib"), brew / "lib", gtk / "lib", out]
system = {
    "libc.so", "libc++_shared.so", "libdl.so", "libm.so", "libpthread.so", "librt.so",
    "libace_napi.z.so", "libace_ndk.z.so", "libhilog_ndk.z.so",
    "libnative_window.so", "libnative_image.so", "libnative_vsync.so",
    "libnative_window_manager.so", "libchild_process.so", "libipc_capi.so",
    "libvulkan.so", "libGLESv3.so",
}

out.mkdir(parents=True, exist_ok=True)
initial = build / "libnative_gtk_demo.so"
needed = [
    gtk / "lib/libgtk-4.so.1",
    gtk / "lib/libadwaita-1.so.0",
    gtk / "lib/libgtk4-layer-shell.so.0",
    gpu / "lib/libwayland-server.so.0",
    app_root / "native-build/native_arm64-v8a/guest-epoxy-install/lib/libguest_epoxy.so.0",
]
needed += [path for path in (initial, root / "ghostty-1.3.1/zig-out/lib/libnative_ghostty.so") if path.is_file()]
seen = set()
missing = set()
while needed:
    current = needed.pop()
    name = "libghostty_ssh.so" if current == Path("/usr/bin/ssh") else current.name
    if name in seen or name in system:
        continue
    seen.add(name)
    if not current.is_file():
        missing.add(name)
        continue
    target = current if current == initial else out / name
    if current != initial:
        target.unlink(missing_ok=True)
        subprocess.run(["sh", "-c", 'cat "$1" > "$2"', "sh", str(current), str(target)], check=True)
        subprocess.run([patchelf, "--remove-rpath", str(target)], check=True)
        subprocess.run([patchelf, "--set-rpath", "$ORIGIN", str(target)], check=True)
        if name == "libghostty_ssh.so":
            archive = app_root / "native-build/ssh-exec.tar"
            subprocess.run(["tar", "-cf", str(archive), "--mode=u+x", "-C", str(out), name], check=True)
            subprocess.run(["tar", "-xf", str(archive), "-C", str(out)], check=True)
    dynamic = subprocess.check_output([str(readelf), "-d", str(target)], text=True)
    if re.search(r"\(NEEDED\).*?\[libepoxy\.so\.0\]", dynamic):
        subprocess.run([
            patchelf, "--replace-needed", "libepoxy.so.0", "libguest_epoxy.so.0", str(target),
        ], check=True)
        dynamic = subprocess.check_output([str(readelf), "-d", str(target)], text=True)
    for dependency in re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic):
        if dependency in seen or dependency in system:
            continue
        source = next((directory / dependency for directory in search if (directory / dependency).is_file()), None)
        if source is None:
            missing.add(dependency)
        else:
            needed.append(source)

if missing:
    raise SystemExit("Missing dependencies: " + ", ".join(sorted(missing)))

# Runtime data (xkb, schemas, icons) and Ghostty data ship through ohos_wl.py.
print(f"Staged {len(seen)} libraries in {out}")
