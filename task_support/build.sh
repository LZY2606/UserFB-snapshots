#!/bin/bash
set -euo pipefail
cc -std=c99 -O2 -I . -c "${TASK_SMACKER_SOURCE:-smacker.c}" -o "$task_build/smacker.o"
c++ -std=c++11 -O2 -I . -I task_support "$@" "$task_build/smacker.o" -o "$task_build/run"
