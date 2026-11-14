#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
for name in t1 t2; do
 bash task_support/build.sh ".task-build/$name" "tests/$name.c"
 ".task-build/$name"
done
bash task_support/build.sh .task-build/legacy_decode task_support/legacy_decode.c
.task-build/legacy_decode
printf '%s\n' BASELINE_OK
