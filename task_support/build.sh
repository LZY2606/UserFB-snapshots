#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++11 -O2 -Wno-deprecated-register -Icodec -Itask_support task_support/main.c tests/golomb_contract.cpp codec/*.c codec/*.cpp -o .task-build/target
