#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O2 -I. task_support/driver.c task_support/tsf_impl.c tests/render_regression.c -lm -o .task-build/target
