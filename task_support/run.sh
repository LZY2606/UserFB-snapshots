#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
build=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$build"' EXIT
if [ "$mode" = target ]; then
  test -s tests/adaptive_blocks.c || { echo 'Missing or empty tests/adaptive_blocks.c' >&2; exit 3; }
fi
cmake -S task_support -B "$build" -DCMAKE_BUILD_TYPE=Release -DCHECK_TARGET="$([ "$mode" = target ] && echo ON || echo OFF)" >"$build/configure.log" 2>&1 || { cat "$build/configure.log"; exit 4; }
targets=(check_baseline check_old_encoders)
if [ "$mode" = target ]; then targets+=(check_target); fi
cmake --build "$build" --target "${targets[@]}" -j 4 >"$build/build.log" 2>&1 || { cat "$build/build.log"; exit 5; }
if [ "$mode" = target ]; then
  rc=0
  "$build/check_target" >"$build/target.log" 2>&1 || rc=$?
  cat "$build/target.log"
  test "$rc" = 0 || exit "$rc"
  awk '
    /^CASE_PASS (optimizer|stream|container) [^ ]+ checks=[1-9][0-9]*$/ { cases[$2]++; total++ }
    /^GROUP_DONE (optimizer|stream|container) cases=[1-9][0-9]* checks=[1-9][0-9]*$/ { done[$2]++ }
    /^TARGET_DONE cases=[1-9][0-9]* checks=[1-9][0-9]*$/ { final++ }
    END { if (cases["optimizer"]<2 || cases["stream"]<2 || cases["container"]<2 || done["optimizer"]!=1 || done["stream"]!=1 || done["container"]!=1 || final!=1 || total<6) exit 1 }
  ' "$build/target.log" || { echo 'Incomplete target groups' >&2; exit 6; }
fi
"$build/check_baseline"
(cd "$build" && ./check_old_encoders > old.log 2>&1) || { cat "$build/old.log"; exit 7; }
cat "$build/old.log"
grep -qx 'OLD_ENCODERS_OK' "$build/old.log"
echo BASELINE_OK
