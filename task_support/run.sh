#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode != baseline && $mode != target ]]; then exit 2; fi
build=.task-build
# This entry owns only .task-build, never source or supplied fixtures.
rm -rf "$build"
mkdir -p "$build/classes"
if [[ $mode == target ]]; then
  test -s tests/BlendWeightSolverTest.java
  test -s gdx/src/com/badlogic/gdx/graphics/g3d/utils/BlendWeightSolver.java
  test -s examples/BlendWeightFit.java
fi
find gdx/src task_support/vendor -type f -name '*.java' | sort > "$build/sources.list"
test -s "$build/sources.list"
javac -encoding UTF-8 -d "$build/classes" @"$build/sources.list" task_support/Current.java
if [[ $mode == target ]]; then
  test -s "$build/classes/com/badlogic/gdx/graphics/g3d/utils/BlendWeightSolver.class"
  javac -encoding UTF-8 -cp "$build/classes" -d "$build/classes" task_support/TaskSuite.java task_support/Checks.java task_support/TaskDriver.java tests/BlendWeightSolverTest.java examples/BlendWeightFit.java
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  java -cp "$build/classes" task_support.TaskDriver > "$log" 2>&1
  cat "$log"
  for group in forward jacobian simplex integration invalid; do
    grep -Eq "^GROUP ${group} checks=[1-9][0-9]* COMPLETE$" "$log"
  done
  grep -Eq '^TARGET_COMPLETE groups=5$' "$log"
  java -cp "$build/classes" BlendWeightFit > "$log" 2>&1
  cat "$log"
  grep -Fx 'BLEND_WEIGHT_FIT_DEMO_OK' "$log"
fi
java -cp "$build/classes" Current
