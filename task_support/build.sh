#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cxx=${CXX:-c++}
objects=()
index=0
while IFS= read -r -d '' source; do
  object=".task-build/core-$index.o"
  "$cxx" -std=c++17 -O1 -Wall -Wextra -I. -c "$source" -o "$object"
  objects+=("$object")
  index=$((index+1))
done < <(find . \( -type d \( -name .git -o -name .task-build -o -name task_support -o -name tests -o -name test -o -name examples \) -prune \) -o \( -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) ! -path ./main.cpp -print0 \))
test "$index" -gt 0
rm -f .task-build/core.a
ar rcs .task-build/core.a "${objects[@]}"
"$cxx" -std=c++17 -O1 -I. task_support/baseline.cpp .task-build/core.a -o .task-build/baseline
