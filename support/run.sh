#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
mode=${1:-baseline}
cxx=${CXX:-c++}
flags=(-std=c++17 -O2 -Wall -Wextra -I. -Isupport)
case "$mode" in
 baseline)
  "$cxx" "${flags[@]}" support/baseline.cpp -o build/baseline
  build/baseline
  "$cxx" "${flags[@]}" examples/raytrace.cpp -o build/raytrace-example
  build/raytrace-example build
  test -s build/depth.pgm
  test -s build/status.pgm
  echo 'BASELINE raytrace-example OK'
  ;;
 target)
  test -s support/target.cpp
  test -s examples/raytrace.cpp
  for z in default alternate; do
   extra=(); if [[ "$z" == alternate ]]; then extra=(-DSIVPERLIN_DEFAULT_Z=0.625); fi
   "$cxx" "${flags[@]}" ${extra[@]+"${extra[@]}"} support/driver.cpp support/target.cpp -o build/target
   for group in crossing ordering geometry limits; do build/target "$group"; done
  done
  "$cxx" "${flags[@]}" examples/raytrace.cpp -o build/raytrace
  build/raytrace build
  test -s build/depth.pgm; test -s build/status.pgm
  bash support/run.sh baseline
  ;;
 *) echo 'usage: bash support/run.sh baseline|target' >&2; exit 2;;
esac
