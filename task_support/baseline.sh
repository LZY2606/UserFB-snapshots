#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
"${CC:-cc}" -std=c99 -O2 -fwrapv -I. -Iilbc/interface -Isignal_processing/include -I.task-build task_support/baseline.c .task-build/libilbc.a -pthread -lm -o .task-build/baseline
.task-build/baseline >.task-build/baseline.out
cat .task-build/baseline.out
if [[ -f task_support/baseline.expected ]]; then diff -u task_support/baseline.expected .task-build/baseline.out; fi
