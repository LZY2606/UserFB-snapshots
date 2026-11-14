#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
cc=${CC:-clang}
sources=(*.c)
"$cc" -std=gnu11 -O1 -I include baseline/current.c "${sources[@]}" -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == baseline ]]; then exit 0; fi
[[ "$mode" == target ]]
"$cc" -std=gnu11 -O1 -I include -I baseline baseline/driver.c test/history_contract.c "${sources[@]}" -o "$build/target"
"$build/target" | tee "$build/target.log"
grep -Eq '^CONTRACT_DONE cases=[1-9][0-9]* groups=' "$build/target.log"
grep -Eq '^GROUP_PASS instructions cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS memory cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS barriers cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS branching cases=[1-9][0-9]*$' "$build/target.log"
