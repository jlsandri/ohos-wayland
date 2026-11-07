#!/bin/sh
set -eu

APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$APP_ROOT/.." && pwd)
BUILD_ROOT=$APP_ROOT/native-build
NATIVE_BUILD=$BUILD_ROOT/native_arm64-v8a
GPU_CROSS=$NATIVE_BUILD/ohos-gpu-arm64-cross.txt
GTK_CROSS=$NATIVE_BUILD/ohos-gtk-arm64-cross.txt
GTK_PREFIX=$BUILD_ROOT/gtk-install
PC_OVERRIDE=$BUILD_ROOT/gtk-pkgconfig
BREW_PREFIX=${BREW_PREFIX:-$(brew --prefix)}
TMPDIR=${TMPDIR:-/data/storage/el2/base/cache}
export TMPDIR

if [ ! -f "$GPU_CROSS" ]; then
    echo "Run sh $APP_ROOT/scripts/build_gpu.sh first" >&2
    exit 1
fi

env -u http_proxy -u https_proxy -u all_proxy -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
    brew install glib cairo fribidi pango graphene gdk-pixbuf libxkbcommon librsvg \
    iso-codes hicolor-icon-theme libfyaml libxmlb itstool gettext ohos-selfsign sassc

mkdir -p "$PC_OVERRIDE" "$NATIVE_BUILD" "$TMPDIR"
FYAML_PREFIX=$(brew --prefix libfyaml)
cat > "$PC_OVERRIDE/libfyaml.pc" <<EOF
prefix=$FYAML_PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libfyaml
Description: Fancy YAML parser library
Version: 0.9.6
Libs: -L\${libdir} -lpthread -lfyaml
Cflags: -I\${includedir}
EOF

python3 - "$GPU_CROSS" "$GTK_CROSS" "$GTK_PREFIX" "$PC_OVERRIDE" <<'PY'
from pathlib import Path
import sys

source, target, prefix, override = sys.argv[1:]
native = str(Path(prefix).parent / 'appstream-native' / 'lib/pkgconfig')
lines = []
for line in Path(source).read_text().splitlines():
    if line.startswith(("c_args = ", "cpp_args = ")):
        line = line.replace(", '-fno-emulated-tls'", f", '-I{prefix}/include', '-fno-emulated-tls'")
    elif line.startswith(("c_link_args = ", "cpp_link_args = ")):
        line = line[:-1] + f", '-L{prefix}/lib']"
    elif line.startswith("pkg_config_path = "):
        line = line[:-1] + f", '{prefix}/lib/pkgconfig']"
        line = line.replace("pkg_config_path = [", f"pkg_config_path = ['{override}', ", 1)
    elif line.startswith("build.pkg_config_path = "):
        line = line[:-1] + f", '{native}']"
    lines.append(line)
content = "\n".join(lines) + "\n"
target_path = Path(target)
if not target_path.exists() or target_path.read_text() != content:
    target_path.write_text(content)
PY

NATIVE_APPSTREAM_PREFIX=$BUILD_ROOT/appstream-native
GETTEXT_PREFIX=$(brew --prefix gettext)
export CFLAGS="-I$GETTEXT_PREFIX/include -I$BREW_PREFIX/include${CFLAGS:+:$CFLAGS}"
export LIBRARY_PATH=$GETTEXT_PREFIX/lib:$BREW_PREFIX/lib:$GTK_PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}
export PKG_CONFIG_PATH=$NATIVE_APPSTREAM_PREFIX/lib/pkgconfig:$PC_OVERRIDE:$BREW_PREFIX/lib/pkgconfig:$BREW_PREFIX/share/pkgconfig:$GTK_PREFIX/lib/pkgconfig

setup_build() {
    name=$1
    source=$2
    shift 2
    build=$BUILD_ROOT/gtk/$name
    if [ -f "$build/build.ninja" ] && [ "$GTK_CROSS" -nt "$build/meson-private/coredata.dat" ]; then
        meson setup --wipe "$build" "$source" --cross-file "$GTK_CROSS" "$@"
    elif [ -f "$build/build.ninja" ]; then
        meson setup --reconfigure "$build" "$source" --cross-file "$GTK_CROSS" "$@"
    else
        meson setup "$build" "$source" --cross-file "$GTK_CROSS" "$@"
    fi
    ninja -C "$build"
    meson install -C "$build"
}

setup_build gtk "$ROOT/thirdparty/gtk" \
    --prefix "$GTK_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
    -Dx11-backend=false -Dwayland-backend=true -Dbroadway-backend=false \
    -Dwin32-backend=false -Dmacos-backend=false -Dandroid-backend=false \
    -Dandroid-runtime=disabled -Dmedia-gstreamer=disabled -Dvulkan=disabled \
    -Df16c=disabled -Dprint-cups=disabled -Dintrospection=disabled \
    -Ddocumentation=false -Dman-pages=false -Dbuild-demos=false \
    -Dbuild-testsuite=false -Dbuild-examples=false -Dbuild-tests=false

if [ ! -d "$ROOT/thirdparty/libadwaita/subprojects/appstream/.git" ]; then
    (CDPATH= cd -- "$ROOT/thirdparty/libadwaita" && meson subprojects download appstream)
fi
if [ ! -d "$ROOT/thirdparty/libadwaita/subprojects/appstream/subprojects/libxmlb/.git" ]; then
    (CDPATH= cd -- "$ROOT/thirdparty/libadwaita/subprojects/appstream" && meson subprojects download libxmlb)
fi

sign_native() {
    signed=$1.signed
    selfsign "$1" "$signed" --force
    tar --mode=u+x -cf "$signed.tar" -C "$(dirname "$1")" "$(basename "$signed")"
    tar -xf "$signed.tar" -C "$(dirname "$1")"
    mv "$signed" "$1"
    rm "$signed.tar"
}

if [ ! -x "$NATIVE_APPSTREAM_PREFIX/bin/appstreamcli" ] || ! "$NATIVE_APPSTREAM_PREFIX/bin/appstreamcli" --version >/dev/null 2>&1; then
    if [ -f "$BUILD_ROOT/appstream-native-build/build.ninja" ]; then
        meson setup --reconfigure "$BUILD_ROOT/appstream-native-build" "$ROOT/thirdparty/libadwaita/subprojects/appstream" \
            --prefix "$NATIVE_APPSTREAM_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
            "-Dc_args=-I$GETTEXT_PREFIX/include -I$BREW_PREFIX/include" \
            "-Dc_link_args=-Wl,-rpath,$NATIVE_APPSTREAM_PREFIX/lib -Wl,-rpath,$GETTEXT_PREFIX/lib -Wl,-rpath,$BREW_PREFIX/lib" \
            -Dsystemd=false -Dapidocs=false -Dinstall-docs=false -Dstemming=false \
            -Dgir=false -Dcompose=false -Ddocs=false -Dman=false \
            -Dbash-completion=false -Dblake3-support=false -Ddisplay-detection=none
    else
        meson setup "$BUILD_ROOT/appstream-native-build" "$ROOT/thirdparty/libadwaita/subprojects/appstream" \
            --prefix "$NATIVE_APPSTREAM_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
            "-Dc_args=-I$GETTEXT_PREFIX/include -I$BREW_PREFIX/include" \
            "-Dc_link_args=-Wl,-rpath,$NATIVE_APPSTREAM_PREFIX/lib -Wl,-rpath,$GETTEXT_PREFIX/lib -Wl,-rpath,$BREW_PREFIX/lib" \
            -Dsystemd=false -Dapidocs=false -Dinstall-docs=false -Dstemming=false \
            -Dgir=false -Dcompose=false -Ddocs=false -Dman=false \
            -Dbash-completion=false -Dblake3-support=false -Ddisplay-detection=none
    fi
    ninja -C "$BUILD_ROOT/appstream-native-build"
    meson install -C "$BUILD_ROOT/appstream-native-build"
    sign_native "$NATIVE_APPSTREAM_PREFIX/lib/libappstream.so.1.2.1"
    sign_native "$NATIVE_APPSTREAM_PREFIX/bin/appstreamcli"
fi
"$NATIVE_APPSTREAM_PREFIX/bin/appstreamcli" --version
export PATH="$NATIVE_APPSTREAM_PREFIX/bin:$PATH"

setup_build appstream "$ROOT/thirdparty/libadwaita/subprojects/appstream" \
    --prefix "$GTK_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
    -Dsystemd=false -Dapidocs=false -Dinstall-docs=false -Dstemming=false \
    -Dgir=false -Dtools=false -Dcompose=false -Ddocs=false -Dman=false \
    -Dbash-completion=false -Dblake3-support=false -Ddisplay-detection=none

setup_build libadwaita "$ROOT/thirdparty/libadwaita" \
    --prefix "$GTK_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
    -Dtests=false -Dexamples=false -Dintrospection=disabled -Dvapi=false \
    -Ddocumentation=false

setup_build gtk4-layer-shell "$ROOT/thirdparty/gtk4-layer-shell" \
    --prefix "$GTK_PREFIX" --libdir lib --wrap-mode=nofallback -Dbuildtype=release \
    -Dtests=false -Dsmoke-tests=false -Dexamples=false -Dintrospection=false \
    -Dvapi=false -Ddocs=false

pkg-config --exists gtk4 libadwaita-1 gtk4-layer-shell-0 appstream
printf '%s\n' "GTK libraries are in $GTK_PREFIX"
