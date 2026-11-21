#!/bin/bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -Iinclude -Ivendor/linear/include task_support/baseline.cpp -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 c++ -std=c++17 -O2 -I. -Iinclude -Ivendor/linear/include -x c++ task_support/driver.c tests/denoise.cpp -o .task-build/target
fi
