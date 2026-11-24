#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
c++ -std=c++11 -O2 -Wno-deprecated-register -Icodec task_support/baseline.cpp codec/*.c codec/*.cpp -o .task-build/baseline
.task-build/baseline
