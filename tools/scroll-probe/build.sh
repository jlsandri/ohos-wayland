#!/bin/sh
# Builds the scroll probe on the device (HiShell) against the runtime's
# libwayland-client and the vendored protocol code.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
protocols=$root/app/entry/src/main/cpp/protocols
mkdir -p "$here/build"
gcc -O2 -o "$here/build/scroll-probe" "$here/probe.c" "$protocols/xdg-shell-protocol.c" \
	-I"$protocols" "$root/app/entry/libs/arm64-v8a/libwayland-client.so.0"
