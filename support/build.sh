#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=${TASK_BUILD_DIR:-$(mktemp -d "$PWD/.task-build.XXXXXX")}
mkdir -p "$build_dir"
cc_args=(-std=c99 -D_DEFAULT_SOURCE -O2 -I. -Isupport -include support/alloc.h)
if [[ ${SCALAR:-float} == double ]]; then cc_args+=(-Dkiss_fft_scalar=double); fi
if [[ ${SANITIZE:-0} == 1 ]]; then cc_args+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
sources=(kiss_fft.c kiss_fftr.c kiss_fftnd.c kiss_fftndr.c support/alloc.c)
