#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O2 -I. task_support/baseline.c freeaptx.c -o .task-build/baseline
.task-build/baseline
