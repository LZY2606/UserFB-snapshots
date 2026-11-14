#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
printf '#define HAVE_BUILTIN_EXPECT 1\n#define ATTRIBUTE_ALIGNED_MAX 16\n' > "$build/config.h"
cc -O2 -std=c99 -I"$build" -Iinclude -Ilibmpeg2 libmpeg2/{alloc,header,decode,slice,motion_comp,idct,cpu_accel,cpu_state}.c support/baseline.c -o "$build/run"
