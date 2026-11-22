#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/LinkedPitchTest.java; fi
bash task_support/build.sh
if [[ $mode == baseline ]]; then
  java -cp .task-build/classes task_support.Baseline
elif [[ $mode == target ]]; then
  find task_tests -name '*.java' -print | LC_ALL=C sort > .task-build/tests.list
  if [[ -d task_examples ]]; then find task_examples -name '*.java' -print | LC_ALL=C sort >> .task-build/tests.list; fi
  javac --release 11 -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/TaskCheck.java @.task-build/tests.list
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  java -cp .task-build/classes task_support.TaskCheck >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in phase stream boundary legacy; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
else
  echo 'unknown mode' >&2
  exit 2
fi
