#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support task_support/main.c tests/adaptive_contract.c -o .task-build/target
