#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build "$b" -j 2
ctest --test-dir "$b" --output-on-failure -R '^(reader|version|nesting)$'
cc -std=c99 -Iinclude checks/baseline.c "$b/libyaml.a" -o "$b/baseline"
"$b/baseline"
if [[ ${1:-baseline} == target ]]; then
 cc -std=c99 -Iinclude -Ichecks checks/target-main.c tests/test_merge_graph.c "$b/libyaml.a" -o "$b/target"
 "$b/target" > "$b/target.log"
 cat "$b/target.log"
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$b/target.log"
fi
