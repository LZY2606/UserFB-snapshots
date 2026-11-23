#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh baseline
c++ -std=c++17 -O2 -pthread -I. task_support/reference_baseline.cpp .task-build/libproduction.a -o .task-build/reference_baseline
.task-build/reference_baseline
