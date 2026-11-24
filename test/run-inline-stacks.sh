#!/bin/sh
# Build the real library from source, run the DWARF4 inline stack
# contract tests, then rebuild and re-run the normal parse/find-pc
# baseline.  Offline only; C++11 and the standard library; no new
# dependencies.
set -eu

cd "$(dirname "$0")/.."
mkdir -p .task-build
cxx=${CXX:-c++}

objects=
for file in elf/*.cc dwarf/*.cc; do
    object=.task-build/$(dirname "$file")_$(basename "$file" .cc).o
    "$cxx" -std=c++11 -O1 -Wall -Ielf -Idwarf -c "$file" -o "$object"
    objects="$objects $object"
done
ar rcs .task-build/libelfin.a $objects

"$cxx" -std=c++11 -O1 -Wall -Ielf -Idwarf \
    test/inline_stacks_contract.cc .task-build/libelfin.a \
    -o .task-build/inline_stacks_contract
.task-build/inline_stacks_contract

# Re-run the normal baseline against the same freshly built library.
"$cxx" -std=c++11 -O1 -Wall -Ielf -Idwarf \
    task_support/normal_baseline.cc .task-build/libelfin.a \
    -o .task-build/normal_baseline
.task-build/normal_baseline

printf '%s\n' INLINE_STACKS_TEST_OK
