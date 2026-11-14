#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
sources=(atomic.c dwarf.c fileline.c posix.c sort.c state.c read.c alloc.c nounwind.c elf.c)
if [[ ${1:-baseline} == target ]]; then
 test -s offline.c
 sources+=(offline.c)
fi
objects=()
for source in "${sources[@]}"; do
 object=".task-build/${source%.c}.o"
 cc -std=c11 -O0 -g -I task_support -I. -c "$source" -o "$object"
 objects+=("$object")
done
rm -f .task-build/libbacktrace.a
ar rcs .task-build/libbacktrace.a "${objects[@]}"
cc -std=c11 -O0 -I task_support -I. task_support/basic_fixture.c task_support/baseline.c .task-build/libbacktrace.a -o .task-build/baseline
