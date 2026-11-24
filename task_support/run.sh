#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ $mode == baseline || $mode == target ]]
if [[ $mode == target ]]; then test -s task_tests/PGSCompositionTest.java; fi
rm -rf .task-build/classes
mkdir -p .task-build/classes .task-build/task-config
javac -encoding ISO-8859-1 -d .task-build/classes task_support/vendor/macify/org/simplericity/macify/eawt/*.java
find src/main/java -name '*.java' | sort > .task-build/sources
javac -encoding UTF-8 -cp .task-build/classes -sourcepath src/main/java:task_support/vendor/scaling:task_support/vendor/cli -d .task-build/classes @.task-build/sources task_support/Baseline.java task_support/Checks.java task_support/TargetDriver.java
java_opts=(-Djava.awt.headless=true "-Duser.home=$PWD/.task-build/task-config" -cp .task-build/classes)
if [[ $mode == target ]]; then
  javac -encoding UTF-8 -cp .task-build/classes -sourcepath src/main/java:task_tests -d .task-build/classes task_tests/PGSCompositionTest.java
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  java "${java_opts[@]}" task_support.TargetDriver > "$output_log" 2>&1
  task_rc=$?
  set -e
  cat "$output_log"
  [[ $task_rc == 0 ]] || exit "$task_rc"
  for group in objects snapshots roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
  java "${java_opts[@]}" Baseline single
else
  java "${java_opts[@]}" Baseline
fi
