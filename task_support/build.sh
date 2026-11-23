#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c11 -O1 -DH264E_MAX_THREADS=0 -include task_support/platform.h -I. -Itask_support/vendor/h264bsd/src)
if [[ $(uname -m) == arm64 || $(uname -m) == aarch64 ]]; then flags+=(-U__aarch64__ -U__ARM_NEON); fi
objects=()
for unit in task_support/codec_impl.c task_support/vendor/h264bsd/src/*.c minih264e_*.c; do
 [[ -f $unit ]] || continue
 case $unit in minih264e_test.c|minih264e_sps_test.c) continue;; esac
 object=.task-build/$(echo "$unit" | tr '/.' '__').o
 cc "${flags[@]}" -c "$unit" -o "$object"
 objects+=("$object")
done
ar rcs .task-build/libvideo.a "${objects[@]}"
cc "${flags[@]}" task_support/baseline.c .task-build/libvideo.a -lm -o .task-build/baseline
cc "${flags[@]}" minih264e_test.c .task-build/libvideo.a -lm -o .task-build/h264e
