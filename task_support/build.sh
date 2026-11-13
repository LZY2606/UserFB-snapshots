#!/bin/bash
set -euo pipefail
mkdir -p "$task_build/ccd"
printf '#define CCD_DOUBLE\n' > "$task_build/ccd/config.h"
for f in src/*.c; do cc -std=c99 -O2 -I src -I "$task_build" -c "$f" -o "$task_build/$(basename "$f" .c).o"; done
c++ -std=c++11 -O2 -I src -I "$task_build" -I task_support "$@" "$task_build"/*.o -lm -o "$task_build/run"
