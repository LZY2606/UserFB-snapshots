#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
cc=${CC:-clang}
flags=(-std=c11 -O1 -DENABLE_OVERRIDE=0 -DRPMALLOC_FIRST_CLASS_HEAPS=1 -I rpmalloc -pthread)
"$cc" "${flags[@]}" baseline/current.c rpmalloc/rpmalloc.c -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == baseline ]]; then exit 0; fi
[[ "$mode" == target ]]
"$cc" "${flags[@]}" -I baseline baseline/driver.c test/quarantine_contract.c rpmalloc/rpmalloc.c -o "$build/target"
"$build/target" | tee "$build/target.log"
grep -Eq '^CONTRACT_DONE cases=[1-9][0-9]* groups=' "$build/target.log"
grep -Eq '^GROUP_PASS retention cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS poison cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS remote cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS heaps cases=[1-9][0-9]*$' "$build/target.log"
