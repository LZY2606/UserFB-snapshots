#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in codec/*.c codec/*.cpp; do
  object=".task-build/$(basename "${source%.*}").o"
  if [[ $source == *.cpp ]]; then
    c++ -std=c++11 -O1 -DDEBUG=1 -DTARGET_RT_LITTLE_ENDIAN=1 -Icodec -I. -c "$source" -o "$object"
  else
    cc -std=c99 -O1 -DDEBUG=1 -DTARGET_RT_LITTLE_ENDIAN=1 -Icodec -I. -c "$source" -o "$object"
  fi
  objects+=("$object")
done
rm -f .task-build/libalac.a
ar rcs .task-build/libalac.a "${objects[@]}"
c++ -std=c++11 -O1 -DDEBUG=1 -DTARGET_RT_LITTLE_ENDIAN=1 -Icodec -I. task_support/baseline.cpp .task-build/libalac.a -lm -o .task-build/baseline
printf 'PRODUCTION_BUILD_OK\n'
