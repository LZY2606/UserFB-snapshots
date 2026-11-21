#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -std=c99 -O1 -I src/h261 -I task_support task_support/baseline.c .task-build/core.a -lm -o .task-build/baseline
.task-build/baseline > .task-build/baseline.out
diff -u task_support/baseline.expected .task-build/baseline.out
cat .task-build/baseline.out
