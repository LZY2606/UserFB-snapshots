#!/bin/bash
set -euo pipefail
test -f Tests/task_turn_cost.cpp
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
export task_build
bash task_support/build.sh task_support/target_main.cpp Tests/task_turn_cost.cpp
"$task_build/run"
bash task_support/baseline.sh
