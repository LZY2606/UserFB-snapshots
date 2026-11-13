#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -I. -c task_support/decoder.c -o "$build/decoder.o"
objects=("$build/decoder.o")
[ -f bcdec_transform.c ] && [ -f bcdec_transform.h ] || { echo "required transform source/header missing" >&2; exit 2; }
if [ -f bcdec_transform.c ]; then
 cc -std=c99 -O1 -I. -c bcdec_transform.c -o "$build/transform.o"
 objects+=("$build/transform.o")
fi
c++ -std=c++17 -O1 -I. task_support/main.cpp task_tests/target.cpp "${objects[@]}" -o "$build/target"
"$build/target" modes | tee "$build/modes.log"
grep -Eq "^GROUP modes cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/modes.log"
"$build/target" partitions | tee "$build/partitions.log"
grep -Eq "^GROUP partitions cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/partitions.log"
"$build/target" anchors | tee "$build/anchors.log"
grep -Eq "^GROUP anchors cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/anchors.log"
"$build/target" integration | tee "$build/integration.log"
grep -Eq "^GROUP integration cases=[1-9][0-9]* assertions=[1-9][0-9]* COMPLETE$" "$build/integration.log"
bash task_support/baseline.sh
