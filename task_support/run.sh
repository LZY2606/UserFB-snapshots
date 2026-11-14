#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build/classes
find jme3-core/src/main/java jme3-core/src/plugins/java jme3-core/src/tools/java -name '*.java' -type f | LC_ALL=C sort > .task-build/sources
if [ "$mode" = target ]; then
  test -s task_tests/SegmentScaleTests.java
  find task_support task_tests -name '*.java' -type f | LC_ALL=C sort >> .task-build/sources
else
  printf '%s\n' task_support/Baseline.java >> .task-build/sources
fi
javac --release 17 -d .task-build/classes @.task-build/sources
if [ "$mode" = target ]; then
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT HUP INT TERM
  set +e
  java -Djava.awt.headless=true -cp .task-build/classes TargetMain > "$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [ "$status" -eq 0 ] || exit "$status"
  for group in matrix legacy lifecycle; do
    grep -Eq "^GROUP $group checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -qx 'TARGET_COMPLETE groups=3' "$output_log"
else
  java -Djava.awt.headless=true -cp .task-build/classes Baseline
fi
