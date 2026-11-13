#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
cxx=${CXX:-c++}
"$cxx" --version | head -1
# Compile fixed source directly. No make rule or code generator is invoked.
objects=
for file in elf/*.cc dwarf/*.cc; do
    object=.task-build/$(dirname "$file")_$(basename "$file" .cc).o
    "$cxx" -std=c++11 -O1 -Ielf -Idwarf -c "$file" -o "$object"
    objects="$objects $object"
done
ar rcs .task-build/libelfin.a $objects
"$cxx" -std=c++11 -O1 -Ielf -Idwarf task_support/normal_baseline.cc .task-build/libelfin.a -o .task-build/normal_baseline
printf '%s\n' BUILD_OK
