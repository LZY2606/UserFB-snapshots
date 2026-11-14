#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support -c task_support/plm.c -o .task-build/plm.o
cc -std=c99 -O2 -I. -Itask_support -c task_support/fixture.c -o .task-build/fixture.o
cc -std=c99 -O2 -I. -Itask_support task_support/baseline.c .task-build/plm.o .task-build/fixture.o -lm -o .task-build/baseline
