#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
c++ -std=c++11 -Iinclude -Iinclude/noise src/*.cpp src/module/*.cpp support/baseline.cpp -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == target ]]; then
  cc -std=c99 -Isupport -c support/runner.c -o "$build/runner.o"
  c++ -std=c++11 -Iinclude -Iinclude/noise src/*.cpp src/module/*.cpp -Isupport support/target.cpp "$build/runner.o" -o "$build/target"
  "$build/target" | tee "$build/target.log"
  for group in primitive graph cache; do
    grep -Eq "^PASS $group/.+" "$build/target.log"
  done
  grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/target.log"
fi
