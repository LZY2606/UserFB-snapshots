#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O0 -I. -c task_support/library.c -o .task-build/library.o
cc -std=c99 -O0 -I. sfotool/*.c -lm -o .task-build/sfotool
c++ -std=c++11 -O0 -I. task_support/baseline.cpp .task-build/library.o -lm -o .task-build/baseline
