#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -Dcimg_display=0 -I. task_support/main.cpp task_tests/target.cpp -o "$build/target"
"$build/target" equations | tee "$build/equations.log"
grep -Eq "^GROUP equations cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/equations.log"
"$build/target" mixed | tee "$build/mixed.log"
grep -Eq "^GROUP mixed cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/mixed.log"
"$build/target" components | tee "$build/components.log"
grep -Eq "^GROUP components cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/components.log"
"$build/target" scale | tee "$build/scale.log"
grep -Eq "^GROUP scale cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/scale.log"
bash task_support/baseline.sh
