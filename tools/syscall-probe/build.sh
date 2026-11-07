#!/bin/sh
# Builds the syscall probe on the device (HiShell).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/build"
gcc -O2 -o "$here/build/syscall-probe" "$here/probe.c"
