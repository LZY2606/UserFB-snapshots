#!/bin/bash
set -euo pipefail
test -f tests/task_sampled_normals.cpp
test -f bc5_normal_optimizer.cpp
test -f bc5_normal_optimizer.h
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
export task_build
bash task_support/build.sh task_support/target_main.cpp tests/task_sampled_normals.cpp bc5_normal_optimizer.cpp
"$task_build/run"
bash task_support/baseline.sh
