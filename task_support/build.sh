#!/bin/bash
set -euo pipefail
mkdir -p .task-build
${CC:-cc} -std=c11 -O2 -I. -Itask_support/generated -c decode.c -o .task-build/decode.o
${CC:-cc} -std=c11 -O2 -I. -Itask_support/generated -c format.c -o .task-build/format.o
${CC:-cc} -std=c11 -O2 -I. -Itask_support/generated decode-test.c .task-build/decode.o .task-build/format.o -o .task-build/upstream-decode
${CC:-cc} -std=c11 -O2 -I. -Itask_support/generated task_support/baseline.c .task-build/decode.o .task-build/format.o -o .task-build/baseline
