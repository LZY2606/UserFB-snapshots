#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
src=(libudis86/itab.c libudis86/decode.c libudis86/syn.c libudis86/syn-intel.c libudis86/syn-att.c libudis86/udis86.c)
cc -std=c99 -DHAVE_STRING_H=1 -I. "${src[@]}" checks/baseline.c -o "$build/baseline"
"$build/baseline"
if [[ ${1:-baseline} == target ]]; then
  test -s libudis86/slice.c && test -s libudis86/slice.h && test -s checks/slice_cases.c
  cc -std=c99 -DHAVE_STRING_H=1 -I. "${src[@]}" libudis86/slice.c checks/slice_cases.c -o "$build/target"
  "$build/target" | tee "$build/target.log"
  for name in byte_alias zero_extend flag_chain conditional old_destination unsupported ownership; do
    grep -Fx "PASS $name" "$build/target.log" >/dev/null
  done
  grep -Fx 'SLICE_OK cases=7' "$build/target.log" >/dev/null
fi
