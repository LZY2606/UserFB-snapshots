#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O2 -I. -c tests/compilation/dr_wav.c -o .task-build/dr_wav.o
cc -std=c99 -O2 -I. task_support/baseline.c .task-build/dr_wav.o -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  shopt -s nullglob
  sources=(tools/*.c)
  if ((${#sources[@]})); then
    cc -std=c99 -O2 -I. "${sources[@]}" .task-build/dr_wav.o -lm -o tools/bwf_join
    for file in "${sources[@]}"; do
      [[ $file == tools/bwf_join.c ]] && continue
      cc -std=c99 -O2 -I. -c "$file" -o ".task-build/production-$(basename "$file" .c).o"
    done
  fi
fi
