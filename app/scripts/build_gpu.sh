#!/bin/sh
set -eu

APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$APP_ROOT/.." && pwd)
OHOS_SDK=${OHOS_SDK:-/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0/sdk/default/openharmony}
TOOL_HOME=${TOOL_HOME:-/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0}
NATIVE_ARCH=arm64-v8a
NATIVE_TARGET=aarch64-linux-ohos
SYSROOT=$OHOS_SDK/native/sysroot
BUILD_DIR=$APP_ROOT/native-build/native_$NATIVE_ARCH
PREFIX=$BUILD_DIR/gpu-install
NATIVE_LIBS=$APP_ROOT/entry/libs/$NATIVE_ARCH
PKG_CONFIG_BIN=${PKG_CONFIG_BIN:-$(command -v pkg-config)}
PATCHELF_BIN=${PATCHELF_BIN:-$(command -v patchelf)}
BREW_PREFIX=${BREW_PREFIX:-$(brew --prefix)}
JOBS=${JOBS:-4}
TMPDIR=${TMPDIR:-/data/storage/el2/base/cache}
PYTHONPATH=$APP_ROOT/native-build/python-deps${PYTHONPATH:+:$PYTHONPATH}
export TMPDIR TOOL_HOME PYTHONPATH
if ! python3 -c 'import yaml, mako, markupsafe, packaging' >/dev/null 2>&1; then
    mkdir -p "$APP_ROOT/native-build/python-deps"
    python3 -m pip install --target "$APP_ROOT/native-build/python-deps" \
        PyYAML Mako MarkupSafe packaging
fi
mkdir -p "$PREFIX" "$NATIVE_LIBS" "$TMPDIR"
cat > "$BUILD_DIR/mktemp" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-d" ]; then
    directory=${2%XXXXXX}$$
    # Storage grants access through the group: a umask 077 directory, as
    # autoconf's config.status asks for, is not writable even by its owner.
    (umask 022 && mkdir "$directory") || exit 1
    echo "$directory"
else
    exec /usr/bin/mktemp "$@"
fi
EOF
mkdir -p "$BUILD_DIR/host-bin"
tar --mode=u+x -cf "$BUILD_DIR/mktemp.tar" -C "$BUILD_DIR" mktemp
tar -xf "$BUILD_DIR/mktemp.tar" -C "$BUILD_DIR/host-bin"

PATH="$BUILD_DIR/host-bin:$PATH"
export PATH

CROSS_FILE=$BUILD_DIR/ohos-gpu-arm64-cross.txt
GUEST_CROSS_FILE=$BUILD_DIR/ohos-guest-gl-arm64-cross.txt
MESA_CROSS_FILE=$BUILD_DIR/ohos-mesa-arm64-cross.txt
HOST_PC=$BUILD_DIR/host-pkgconfig
FFI_PREFIX=$BUILD_DIR/libffi/install
cat > "$CROSS_FILE" <<EOF
[binaries]
c = '$OHOS_SDK/native/llvm/bin/clang'
cpp = '$OHOS_SDK/native/llvm/bin/clang++'
ar = '$OHOS_SDK/native/llvm/bin/llvm-ar'
strip = '$OHOS_SDK/native/llvm/bin/llvm-strip'
pkg-config = '$PKG_CONFIG_BIN'

[built-in options]
c_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-I$PREFIX/include', '-I$PREFIX/include/libdrm', '-I$FFI_PREFIX/include', '-I$BREW_PREFIX/include', '-fno-emulated-tls', '-D_GNU_SOURCE', '-U__OHOS_FAMILY__', '-DDETECT_OS_OHOS=0']
cpp_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-I$PREFIX/include', '-I$PREFIX/include/libdrm', '-I$FFI_PREFIX/include', '-I$BREW_PREFIX/include', '-fno-emulated-tls', '-D_GNU_SOURCE', '-U__OHOS_FAMILY__', '-DDETECT_OS_OHOS=0']
c_link_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$PREFIX/lib', '-L$FFI_PREFIX/lib', '-L$BREW_PREFIX/lib']
cpp_link_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$PREFIX/lib', '-L$FFI_PREFIX/lib', '-L$BREW_PREFIX/lib']
pkg_config_path = ['$PREFIX/lib/pkgconfig', '$PREFIX/share/pkgconfig', '$FFI_PREFIX/lib/pkgconfig', '$BREW_PREFIX/lib/pkgconfig', '$BREW_PREFIX/share/pkgconfig']
build.pkg_config_path = ['$HOST_PC']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
sed "s/'-DDETECT_OS_OHOS=0'/'-DDETECT_OS_OHOS=0', '-U__OHOS__', '-DWINEHUA_EGL_ONLY=1'/g" \
    "$CROSS_FILE" > "$GUEST_CROSS_FILE"
cat "$CROSS_FILE" > "$MESA_CROSS_FILE"

export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$FFI_PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig

build_host_scanner() {
    host_scanner=$BUILD_DIR/wayland-host/src/wayland-scanner
    if [ ! -x "$host_scanner" ]; then
        env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR meson setup "$BUILD_DIR/wayland-host" \
            "$ROOT/upstream/thirdparty/wayland" --prefix "$BUILD_DIR/host-tools" --libdir lib \
            -Dlibraries=false -Dscanner=true -Ddtd_validation=false \
            -Ddocumentation=false -Dtests=false --buildtype=release
        ninja -C "$BUILD_DIR/wayland-host"
    fi
    mkdir -p "$HOST_PC"
    scanner_version=$($host_scanner --version 2>&1 | cut -d' ' -f2)
    cat > "$HOST_PC/wayland-scanner.pc" <<EOF
prefix=$BUILD_DIR/wayland-host/src
bindir=\${prefix}
Name: Wayland Scanner
Description: Build-machine protocol scanner
Version: $scanner_version
wayland_scanner=\${bindir}/wayland-scanner
EOF
}

setup_build() {
    source_dir=$1
    build_dir=$2
    shift 2
    cross_file=${BUILD_CROSS_FILE:-$CROSS_FILE}
    if [ -f "$build_dir/build.ninja" ]; then
        meson setup --reconfigure "$build_dir" "$source_dir" --cross-file "$cross_file" "$@"
    else
        meson setup "$build_dir" "$source_dir" --cross-file "$cross_file" "$@"
    fi
    ninja -C "$build_dir"
    meson install -C "$build_dir"
}

build_libffi() {
    if [ -f "$FFI_PREFIX/lib/pkgconfig/libffi.pc" ]; then
        echo "libffi build files already installed"
        return
    fi
    source_dir=$ROOT/upstream/thirdparty/libffi
    build_dir=$BUILD_DIR/libffi
    mkdir -p "$build_dir"
    if [ ! -f "$source_dir/configure" ]; then
        (CDPATH= cd -- "$source_dir" && ./autogen.sh)
    fi
    CDPATH= cd -- "$build_dir"
    CC="$OHOS_SDK/native/llvm/bin/clang --target=$NATIVE_TARGET --sysroot=$SYSROOT" \
    AR="$OHOS_SDK/native/llvm/bin/llvm-ar" \
    RANLIB="$OHOS_SDK/native/llvm/bin/llvm-ranlib" \
    NM="$OHOS_SDK/native/llvm/bin/llvm-nm" \
    LD="$OHOS_SDK/native/llvm/bin/ld.lld" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld --target=$NATIVE_TARGET --sysroot=$SYSROOT" \
        "$source_dir/configure" --build=x86_64-linux-gnu --host=aarch64-linux-gnu \
        --prefix="$FFI_PREFIX" --disable-docs
    make -j"$JOBS"
    make install
}

build_wayland() {
    if [ -f "$PREFIX/lib/libwayland-egl.so.1" ]; then
        echo "GPU Wayland already built"
        return
    fi
    setup_build "$ROOT/upstream/thirdparty/wayland" "$BUILD_DIR/wayland-gpu" \
        --prefix "$PREFIX" --libdir lib -Ddocumentation=false -Dtests=false -Dscanner=false
}

build_protocols() {
    if [ -f "$PREFIX/share/wayland-protocols/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml" ]; then
        echo "Wayland protocols already installed"
        return
    fi
    build_dir=$BUILD_DIR/wayland-protocols-gpu
    if [ -f "$build_dir/build.ninja" ]; then
        meson setup --reconfigure "$build_dir" "$ROOT/upstream/thirdparty/wayland-protocols" \
            --prefix "$PREFIX" -Dtests=false
    else
        meson setup "$build_dir" "$ROOT/upstream/thirdparty/wayland-protocols" \
            --prefix "$PREFIX" -Dtests=false
    fi
    ninja -C "$build_dir"
    meson install -C "$build_dir"
}

build_libdrm() {
    if [ -f "$PREFIX/lib/libdrm.so.2" ]; then
        echo "libdrm already built"
        return
    fi
    setup_build "$ROOT/upstream/thirdparty/libdrm" "$BUILD_DIR/libdrm-gpu" \
        --prefix "$PREFIX" --libdir lib -Dbuildtype=release \
        -Dtests=false -Dinstall-test-programs=false -Dcairo-tests=disabled \
        -Dman-pages=disabled -Dvalgrind=disabled -Dudev=false \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
        -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled
}

# GTK links against this libepoxy; stage_gtk.py rebinds GTK to guest epoxy.
build_libepoxy() {
    if [ -f "$PREFIX/lib/libepoxy.so.0" ]; then
        echo "libepoxy already built"
        return
    fi
    setup_build "$ROOT/upstream/thirdparty/libepoxy" "$BUILD_DIR/libepoxy-gpu" \
        --prefix "$PREFIX" --libdir lib -Dglx=no -Degl=yes -Dx11=false \
        -Ddocs=false -Dtests=false
}

build_guest_epoxy() {
    guest_prefix=$BUILD_DIR/guest-epoxy-install
    guest_build=$BUILD_DIR/guest-libepoxy
    if [ -f "$guest_build/build.ninja" ]; then
        meson setup --wipe "$guest_build" "$ROOT/upstream/thirdparty/libepoxy" \
            --cross-file "$GUEST_CROSS_FILE" --prefix "$guest_prefix" --libdir lib \
            -Dglx=no -Degl=yes -Dx11=false -Ddocs=false -Dtests=false
    else
        meson setup "$guest_build" "$ROOT/upstream/thirdparty/libepoxy" \
            --cross-file "$GUEST_CROSS_FILE" --prefix "$guest_prefix" --libdir lib \
            -Dglx=no -Degl=yes -Dx11=false -Ddocs=false -Dtests=false
    fi
    ninja -C "$guest_build"
    meson install -C "$guest_build"
    guest_library=$guest_prefix/lib/libepoxy.so.0.0.0
    "$PATCHELF_BIN" --set-soname libguest_epoxy.so.0 "$guest_library"
    cat "$guest_library" > "$guest_prefix/lib/libguest_epoxy.so.0"
    cat "$guest_library" > "$guest_prefix/lib/libguest_epoxy.so"
}

build_mesa() {
    python3 -c 'import mako, markupsafe'
    # Mesa's OHOS platform logs through HiLog; the SDK ships the library
    # but no pkg-config file for it.
    mkdir -p "$PREFIX/lib/pkgconfig"
    cat > "$PREFIX/lib/pkgconfig/libhilog.pc" <<EOF
Name: libhilog
Description: HarmonyOS HiLog
Version: 1.0
Libs: -lhilog_ndk.z
EOF
    BUILD_CROSS_FILE=$MESA_CROSS_FILE setup_build "$ROOT/upstream/thirdparty/mesa" \
        "$BUILD_DIR/mesa-gl-zink-loader-gpu" --prefix "$PREFIX" --libdir lib -Dbuildtype=release \
        -Dplatforms=wayland,ohos -Degl-native-platform=ohos \
        -Dgallium-drivers=zink -Dvulkan-drivers= \
        -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
        -Dgbm=disabled -Dglx=disabled -Dtools= -Dglvnd=disabled \
        -Dshared-glapi=enabled -Dshader-cache=enabled -Dllvm=disabled \
        -Ddraw-use-llvm=false -Dexpat=disabled -Dxmlconfig=disabled -Dlmsensors=disabled \
        -Dvideo-codecs=
}

stage_libraries() {
    set -- "$PREFIX/lib/"libgallium-*.so
    [ -f "$1" ]
    cat "$PREFIX/lib/libEGL.so" > "$PREFIX/lib/libEGL.so.1"
    "$PATCHELF_BIN" --set-soname libEGL.so.1 "$PREFIX/lib/libEGL.so.1"
    rm -f "$NATIVE_LIBS/libEGL.so"
    for source in "$PREFIX/lib/"*.so*; do
        [ -f "$source" ] || continue
        name=${source##*/}
        [ "$name" != libEGL.so ] || continue
        "$PATCHELF_BIN" --remove-rpath "$source"
        "$PATCHELF_BIN" --set-rpath '$ORIGIN' "$source"
        cat "$source" > "$NATIVE_LIBS/$name"
    done
    cat "$BUILD_DIR/guest-epoxy-install/lib/libguest_epoxy.so.0" > \
        "$NATIVE_LIBS/libguest_epoxy.so.0"
    [ -f "$NATIVE_LIBS/libguest_epoxy.so.0" ]
    [ -f "$NATIVE_LIBS/libEGL.so.1" ]
}

build_host_scanner
build_libffi
build_wayland
build_protocols
build_libdrm
build_libepoxy
build_guest_epoxy
build_mesa
stage_libraries

echo "GPU host and native arm64 Mesa are in $PREFIX"
echo "HAP libraries are in $NATIVE_LIBS"
