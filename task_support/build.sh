#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in mkvmuxer/*.cc mkvparser/*.cc common/*.cc; do
  case "$source" in *_test.cc) continue ;; esac
  object=".task-build/${source//\//_}.o"
  c++ -std=c++11 -Wall -Wextra -I. -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libwebm.a
ar rcs .task-build/libwebm.a "${objects[@]}"
c++ -std=c++11 -Wall -Wextra -I. task_support/baseline.cc .task-build/libwebm.a -o .task-build/baseline
