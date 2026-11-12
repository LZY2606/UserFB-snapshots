#!/bin/bash
set -euo pipefail
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
export task_build
bash task_support/build.sh task_support/baseline.cpp
"$task_build/run"
bash task_support/build.sh task_support/legacy_blocks.cpp
"$task_build/run"
