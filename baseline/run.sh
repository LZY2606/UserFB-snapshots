#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
cc=${CC:-clang}
bash baseline/build-library.sh "$build"
"$cc" -std=gnu11 -DHAVE_STDBOOL_H=1 -I libyara/include baseline/current.c "$build/libyara.a" -pthread -lm -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == baseline ]]; then exit 0; fi
[[ "$mode" == target ]]
"$cc" -std=gnu11 -DHAVE_STDBOOL_H=1 -I libyara/include -I baseline baseline/driver.c tests/condition_trace_contract.c "$build/libyara.a" -pthread -lm -o "$build/target"
"$build/target" | tee "$build/target.log"
grep -Eq '^CONTRACT_DONE cases=[1-9][0-9]* groups=' "$build/target.log"
grep -Eq '^GROUP_PASS shortcircuit cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS quantifiers cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS decisions cases=[1-9][0-9]*$' "$build/target.log"
grep -Eq '^GROUP_PASS persistence cases=[1-9][0-9]*$' "$build/target.log"
