#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/production
rm -f .task-build/production/*.o
c++ -std=c++17 -O2 -I. -c task_support/production.cpp -o .task-build/production/header.o
n=0
while IFS= read -r -d '' source; do
  n=$((n+1))
  c++ -std=c++17 -O2 -I. -c "$source" -o ".task-build/production/extra-$n.o"
done < <(find . -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.c' \) ! -path './tests/*' ! -path './examples/*' ! -path './task_support/*' ! -path './.task-build/*' -print0)
c++ -std=c++17 -O2 -I. task_support/baseline.cpp .task-build/production/*.o -o .task-build/baseline
