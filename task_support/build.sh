#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for lib in liveMedia groupsock UsageEnvironment BasicUsageEnvironment; do
  rm -rf ".task-build/$lib"
  find "$lib" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' \) -print0 | xargs -0 -n 1 -P 4 bash task_support/compile-one.sh
  rm -f ".task-build/$lib.a"
  ar rcs ".task-build/$lib.a" .task-build/"$lib"/*.o
done
