#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/legacy.sh
mkdir -p .task-build
bash task_support/build.sh .task-build/stack_decode task_support/stack_decode.c
.task-build/stack_decode
