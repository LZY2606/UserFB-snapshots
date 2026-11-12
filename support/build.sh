#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
printf '#define HAVE_LIMITS_H 1\n#define HAVE_ASSERT_H 1\n#define HAVE_STDINT_H 1\n#define SIZEOF_INT 4\n#define SIZEOF_LONG 8\n#define SIZEOF_LONG_LONG 8\n' > "$build/config.h"
src=(fixed.c bit.c timer.c stream.c frame.c synth.c layer12.c layer3.c huffman.c)
if [[ ${2:-baseline} == target ]]; then
  src+=(support/target_main.c tests/mixed8k_contract.c)
  testflags=(-DMAD_MIXED8K_TEST)
else
  src+=(support/baseline.c)
  testflags=()
fi
# ${arr[@]+...} keeps the expansion empty under set -u on bash 3.2
cc -O2 -std=c99 -DHAVE_CONFIG_H -DFPM_64BIT ${testflags[@]+"${testflags[@]}"} -I"$build" -I. -Isupport "${src[@]}" -lm -o "$build/run"
