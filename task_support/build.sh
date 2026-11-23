#!/bin/bash
set -euo pipefail
mode=${1:?}
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s lib/fuzzer/FuzzerEpoch.h
  test -s lib/fuzzer/FuzzerEpoch.cpp
fi
rm -f .task-build/Fuzzer*.o
for file in lib/fuzzer/Fuzzer*.cpp; do
  clang++ -std=c++11 -O0 -g0 -pthread -c "$file" -o ".task-build/$(basename "${file%.cpp}").o"
done
rm -f .task-build/libFuzzer.a
ar rcs .task-build/libFuzzer.a .task-build/Fuzzer*.o
clang++ -std=c++11 -O0 -fsanitize-coverage=inline-8bit-counters,pc-table task_support/benign.cpp .task-build/libFuzzer.a -pthread -o .task-build/benign
