#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
sdk=${OHOS_SDK:-$(brew --prefix ohos-sdk-native)/sysroot}
mkdir -p "$here/build"
gcc -O2 -I"$sdk/usr/include" -o "$here/build/hover-probe" "$here/probe.c" \
    "$sdk/usr/lib/aarch64-linux-ohos/libohinput.so"
