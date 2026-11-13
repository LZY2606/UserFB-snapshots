#!/bin/bash
set -euo pipefail
test -f src/testsuites/task_cast.cpp
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
export task_build
bash task_support/build.sh task_support/target_main.cpp src/testsuites/task_cast.cpp
"$task_build/run"
bash task_support/baseline.sh
