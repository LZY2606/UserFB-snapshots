#!/bin/bash
set -euo pipefail
mkdir -p .task-build
inputs=$(find liveMedia groupsock UsageEnvironment BasicUsageEnvironment -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.hh' -o -name '*.h' \) -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
inputs="$inputs $(shasum -a 256 task_support/compile-one.sh task_support/build.sh) ${CC:-cc} ${CXX:-c++}"
if [[ -f .task-build/source-digest && $(cat .task-build/source-digest) == "$inputs" ]]; then
  complete=1
  for lib in liveMedia groupsock UsageEnvironment BasicUsageEnvironment; do
    [[ -s .task-build/$lib.a ]] || complete=0
  done
  [[ $complete == 1 ]] && exit 0
fi
for lib in liveMedia groupsock UsageEnvironment BasicUsageEnvironment; do
  rm -rf ".task-build/$lib"
  find "$lib" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' \) -print0 | xargs -0 -n 1 -P 4 bash task_support/compile-one.sh
  rm -f ".task-build/$lib.a"
  ar rcs ".task-build/$lib.a" .task-build/"$lib"/*.o
done
printf '%s\n' "$inputs" > .task-build/source-digest
