#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=()
while IFS= read -r source; do sources+=("$source"); done < <(find src -type f \( -name '*.c' -o -name '*.cxx' -o -name '*.cpp' \) | sort)
objects=()
for source in "${sources[@]}"; do objects+=(".task-build/${source}.o"); done
make -s -f task_support/Makefile "${objects[@]}" .task-build/task_support/baseline.cpp.o .task-build/task_support/old_tests.c.o .task-build/test/cutest/CuTest.c.o .task-build/test/testutils.c.o .task-build/test/c/insert_knot.c.o .task-build/test/c/rmf.c.o
rm -f .task-build/libproduction.a
ar rcs .task-build/libproduction.a "${objects[@]}"
c++ .task-build/task_support/baseline.cpp.o .task-build/libproduction.a -lm -o .task-build/baseline
c++ .task-build/task_support/old_tests.c.o .task-build/test/cutest/CuTest.c.o .task-build/test/testutils.c.o .task-build/test/c/insert_knot.c.o .task-build/test/c/rmf.c.o .task-build/libproduction.a -lm -o .task-build/old_tests
