#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
printf '#define HAVE_BUILTIN_EXPECT 1\n#define ATTRIBUTE_ALIGNED_MAX 16\n#if defined(__aarch64__)\n#define ARCH_AARCH64 1\n#endif\n' > "$build/config.h"
src=(libmpeg2/alloc.c libmpeg2/header.c libmpeg2/decode.c libmpeg2/slice.c libmpeg2/motion_comp.c libmpeg2/idct.c libmpeg2/cpu_accel.c libmpeg2/cpu_state.c)
for f in libmpeg2/*_neon.c; do [[ ! -f "$f" ]] || src+=("$f"); done
if [[ ${2:-baseline} == target ]]; then
 src+=(support/target_main.c tests/aarch64_contract.c)
else
 src+=(support/baseline.c)
fi
cc -O3 -std=c99 -I"$build" -Iinclude -Ilibmpeg2 -Isupport "${src[@]}" -o "$build/run"
