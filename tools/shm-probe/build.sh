#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/build"
gcc -O2 -o "$here/build/shm-probe" "$here/probe.c"
