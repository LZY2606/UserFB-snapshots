#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
cc -std=c11 -O2 -DSBC_SYNTHESIS_CONTRACT -Iinclude -Isrc -Itask_support task_support/main.c tests/synthesis_contract.c src/bits.c -lm -o .task-build/target
