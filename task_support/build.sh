#!/bin/bash
set -euo pipefail
mode=$1
mkdir -p .task-build
mode_stamp=.task-build/build-mode
if [[ -f "$mode_stamp" && "$(<"$mode_stamp")" != "$mode" ]]; then
  # The support makefile stores both flavors in the same object directory.
  rm -rf .task-build
  mkdir -p .task-build
fi
printf '%s\n' "$mode" > "$mode_stamp"
if [[ $mode == target ]]; then
 test -s src/core/chuck_beat.h
 test -s src/core/chuck_beat.cpp
 test -s examples/beat_timeline.ck
fi
make -f task_support/core.mk MODE="$mode" -j4
flags=$(cat task_support/flags.txt)
c++ $flags -std=c++17 task_support/baseline.cpp .task-build/libchuck.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
 c++ $flags -std=c++17 -include src/core/chuck_beat.h task_support/beat_probe.cpp .task-build/src/core/chuck_beat.o .task-build/libchuck.a -pthread -o .task-build/beat_probe
 .task-build/beat_probe
 c++ $flags -std=c++17 -include src/core/chuck_beat.h task_support/render_example.cpp .task-build/src/core/chuck_beat.o .task-build/libchuck.a -pthread -o .task-build/render_example
 example_out=$(mktemp)
 rm -f "$example_out"
 trap 'rm -f "$example_out"' EXIT
 .task-build/render_example examples/beat_timeline.ck "$example_out"
 test -s "$example_out"
fi
