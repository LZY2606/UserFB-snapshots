#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O2 -I. -c task_support/production.c -o .task-build/tsf.o
cc -std=c99 -O2 -I. task_support/baseline.c .task-build/tsf.o -lm -o .task-build/baseline
