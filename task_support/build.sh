#!/bin/bash
set -euo pipefail
mkdir -p .task-build/objects
objects=()
for src in elf/*.cc dwarf/*.cc; do
 obj=".task-build/objects/${src//\//_}.o"
 "${CXX:-c++}" -std=c++11 -O0 -I. -Ielf -Idwarf -c "$src" -o "$obj"
 objects+=("$obj")
done
ar rcs .task-build/libreader.a "${objects[@]}"
"${CXX:-c++}" -std=c++11 -O0 -I. task_support/baseline.cc .task-build/libreader.a -o .task-build/baseline
