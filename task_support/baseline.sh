#!/bin/bash
set -euo pipefail
cc -std=c99 -O1 -Isrc/y262 task_support/baseline.c .task-build/core/*.o -lm -pthread -o .task-build/baseline
.task-build/baseline
