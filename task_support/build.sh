#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c11 -O1 -DH264E_MAX_THREADS=0 -include task_support/platform.h -Isrc -Itask_support/vendor/minih264)
if [[ $(uname -m) == arm64 || $(uname -m) == aarch64 ]]; then flags+=(-U__aarch64__ -U__ARM_NEON); fi
objects=()
for unit in src/*.c task_support/fixture_encoder.c; do
 object=.task-build/$(echo "$unit" | tr '/.' '__').o
 cc "${flags[@]}" -c "$unit" -o "$object"
 objects+=("$object")
done
cc "${flags[@]}" task_support/baseline.c "${objects[@]}" -lm -o .task-build/baseline
