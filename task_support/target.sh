#!/bin/bash
set -euo pipefail
test -f tests/task_video_model.cpp
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
export task_build
bash task_support/build.sh task_support/target_main.cpp tests/task_video_model.cpp
"$task_build/run"
bash task_support/baseline.sh
node task_support/mutate.mjs
