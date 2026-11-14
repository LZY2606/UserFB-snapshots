#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
export CCACHE_DISABLE=1
flags=(-std=c99 -O2 -I. -Iinclude -Isrc -DHAVE_STDBOOL_H=1 -DHAVE_LRINT=1 -DPACKAGE=\"libsamplerate\" -DVERSION=\"0.2.2\" -DENABLE_SINC_FAST_CONVERTER=1 -DENABLE_SINC_MEDIUM_CONVERTER=1 -DENABLE_SINC_BEST_CONVERTER=1)
objects=()
for source in src/*.c; do
  object=.task-build/$(basename "${source%.c}").o
  cc "${flags[@]}" -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/libsamplerate.a "${objects[@]}"
cc "${flags[@]}" task_support/baseline.c .task-build/libsamplerate.a -lm -o .task-build/baseline
