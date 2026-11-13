#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -s task_tests/collision.cpp ]] || { echo 'missing task_tests/collision.cpp' >&2; exit 2; }
bash task_support/build.sh
${CXX:-c++} -std=c++11 -O1 -g -Isrc -I. task_support/main.cpp task_tests/collision.cpp build/task/libmeshoptimizer.a -o build/task/target
bash task_support/groups.sh
[[ -s examples/collision_simplify.cpp ]] || { echo 'missing examples/collision_simplify.cpp' >&2; exit 2; }
${CXX:-c++} -std=c++11 -O1 -g -Isrc -I. examples/collision_simplify.cpp build/task/libmeshoptimizer.a -o build/task/example
mkdir -p build/task/example-output
rm -f build/task/example-output/before.obj build/task/example-output/after.obj
build/task/example build/task/example-output
for obj in before after; do
  awk '$1=="v" {v++} $1=="f" {f++} END {if(v<3 || f<1) exit 1}' "build/task/example-output/$obj.obj"
done
printf '%s\n' EXAMPLE_COMPLETE
build/task/baseline
build/task/old_tests
printf '%s\n' COLLISION_TARGET_COMPLETE
