#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=(libudis86/*.c task_support/generated/libudis86/itab.c)
"${CC:-cc}" -std=c99 -O0 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 "${sources[@]}" task_support/baseline.c -o .task-build/baseline
"${CC:-cc}" -std=c99 -O0 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 "${sources[@]}" tests/libcheck.c -o .task-build/libcheck
