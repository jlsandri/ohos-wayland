#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
protocols=$root/app/entry/src/main/cpp/protocols
gpu=$root/app/native-build/native_arm64-v8a/gpu-install
pool=$root/app/entry/libs/arm64-v8a
mkdir -p "$here/build"
gcc -O2 -o "$here/build/wl-gl-probe" "$here/probe.c" "$protocols/xdg-shell-protocol.c" \
    -I"$protocols" -I"$gpu/include" "$pool/libwayland-client.so.0" "$pool/libwayland-egl.so.1" \
    "$pool/libEGL.so.1" -lm
