#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sig=$(find src -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.c' \) -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
if [[ ! -f .task-build/signature || $(cat .task-build/signature) != "$sig" ]]; then
 objects=()
 for file in src/third-party/*.c; do
  obj=.task-build/$(basename "$file" .c).o
  cc -std=gnu99 -O1 -c "$file" -o "$obj"
  objects+=("$obj")
 done
 for file in src/*.cpp src/thread/*.cpp; do
  case "$file" in src/Main.cpp|src/Events.cpp|src/ThreadBarrier.cpp) continue;; esac
  obj=.task-build/$(basename "$file" .cpp).o
  c++ -std=c++11 -O1 -pthread -Isrc -c "$file" -o "$obj"
  objects+=("$obj")
 done
 rm -f .task-build/libtaskcore.a
 ar rcs .task-build/libtaskcore.a "${objects[@]}"
 printf '%s' "$sig" >.task-build/signature
fi
c++ -std=c++11 -O1 -pthread -Isrc task_support/baseline.cpp .task-build/libtaskcore.a -o .task-build/baseline
