#!/bin/bash
set -euo pipefail
mkdir -p .task-build
printf '#define SOUNDTOUCH_FLOAT_SAMPLES 1\n#define SOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS 1\n' > .task-build/soundtouch_config.h
flags=(-std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -Iinclude -Isource/SoundTouch -I.task-build -I.)
objects=()
for src in source/SoundTouch/*.cpp; do
  obj=".task-build/$(basename "${src%.cpp}").o"
  c++ "${flags[@]}" -c "$src" -o "$obj"
  objects+=("$obj")
done
ar rcs .task-build/libsoundtouch.a "${objects[@]}"
c++ "${flags[@]}" task_support/baseline.cpp .task-build/libsoundtouch.a -o .task-build/baseline
