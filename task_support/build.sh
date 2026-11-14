#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
find include libsrc -type f -exec cksum {} + >.task-build/source-current
cksum task_support/build.sh >>.task-build/source-current
if [[ ! -f .task-build/libtrimesh.a || ! -f .task-build/source-built ]] || ! cmp -s .task-build/source-current .task-build/source-built; then
  rm -f .task-build/obj/*.o .task-build/libtrimesh.a
  for source_file in libsrc/*.cc; do
    case "$source_file" in libsrc/GLCamera.cc|libsrc/GLManager.cc) continue;; esac
    object_file=.task-build/obj/$(basename "${source_file%.cc}").o
    c++ -std=c++11 -O0 -Iinclude -c "$source_file" -o "$object_file"
  done
  ar rcs .task-build/libtrimesh.a .task-build/obj/*.o
  cp .task-build/source-current .task-build/source-built
fi
c++ -std=c++11 -O0 -Iinclude task_support/baseline.cc .task-build/libtrimesh.a -o .task-build/baseline
