#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c11 -O2 -Iinclude -Isrc task_support/baseline.c src/sbc.c src/bits.c -o .task-build/baseline
.task-build/baseline
