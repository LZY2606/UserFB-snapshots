#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
sources=(rg_etc1.cpp)
if [ -f rg_etc2.cpp ]; then sources+=(rg_etc2.cpp); fi
c++ -std=c++17 -O2 -I. task_support/main.cpp task_tests/target.cpp "${sources[@]}" -o "$build/target"
"$build/target" decode | tee "$build/decode.log"
grep -Eq "^GROUP decode cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/decode.log"
"$build/target" search | tee "$build/search.log"
grep -Eq "^GROUP search cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/search.log"
"$build/target" images | tee "$build/images.log"
grep -Eq "^GROUP images cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/images.log"
"$build/target" compatibility | tee "$build/compatibility.log"
grep -Eq "^GROUP compatibility cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/compatibility.log"
bash task_support/baseline.sh
