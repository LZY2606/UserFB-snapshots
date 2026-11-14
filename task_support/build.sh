#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support task_support/main.c tests/layer2_contract.c -lm -o .task-build/target
