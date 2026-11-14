#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
objects=()
for src in src/h261/*.c; do
  case $src in */h261_main.c|*/sys_win32.c) continue;; esac
  cc -std=c99 -O1 -fno-strict-aliasing -DH261_PORTABLE -I src/h261 -c "$src" -o ".task-build/$(basename "${src%.c}").o"
  objects+=(".task-build/$(basename "${src%.c}").o")
done
cc -std=c99 -O1 -I. -I src/h261 -c task_support/fixture.c -o .task-build/fixture.o
rm -f .task-build/core.a
ar rcs .task-build/core.a "${objects[@]}" .task-build/fixture.o
