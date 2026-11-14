#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/legacy.sh
mkdir -p .task-build
bash task_support/build.sh .task-build/decode_inputs task_support/decode_inputs.c
.task-build/decode_inputs
printf '%s\n' BASELINE_OK
