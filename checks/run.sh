#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|legacy) ;; target) test -s checks/target.c || { echo 'Missing nonempty checks/target.c' >&2; exit 2; } ;; *) exit 2 ;; esac
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DNANOARROW_BUILD_TESTS=OFF -DNANOARROW_IPC=OFF -DNANOARROW_DEVICE=OFF -DNANOARROW_TESTING=OFF -DNANOARROW_BUNDLE=OFF >"$b/config.log" 2>&1 || { cat "$b/config.log" >&2; exit 1; }
cmake --build "$b/build" --target nanoarrow_static -j2 >"$b/build.log" 2>&1 || { cat "$b/build.log" >&2; exit 1; }
includes=(-Isrc "-I$b/build/src")
lib="$b/build/libnanoarrow_static.a"
cc -std=c99 "${includes[@]}" checks/baseline.c "$lib" -o "$b/baseline"
legacy() {
  local g=vendor/googletest
  c++ -std=c++14 -pthread "${includes[@]}" -I"$g/googletest/include" -I"$g/googletest" \
    -I"$g/googlemock/include" -I"$g/googlemock" \
    src/nanoarrow/common/array_test.cc "$g/googletest/src/gtest-all.cc" \
    "$g/googletest/src/gtest_main.cc" "$g/googlemock/src/gmock-all.cc" \
    "$lib" -o "$b/legacy"
  "$b/legacy" --gtest_filter='ArrayTest.ArrayAppendStorageFromArrayView*' \
    --gtest_output="xml:$b/legacy.xml" | tee "$b/legacy.log"
  # GTest 1.14 returns success on a zero-match filter. Require real passing cases.
  awk '/<testsuites / { if ($0 ~ /tests="[1-9][0-9]*"/ && $0 ~ /failures="0"/ && $0 ~ /disabled="0"/) ok=1 } END {exit !ok}' "$b/legacy.xml"
  for name in Dictionary NestedAndSliced FixedWidthSlicedValidity Errors; do
    grep -F "[       OK ] ArrayTest.ArrayAppendStorageFromArrayView$name (" "$b/legacy.log" >/dev/null
  done
}
case "$mode" in
baseline) "$b/baseline" ;;
legacy) legacy ;;
target)
  cc -std=c99 -Ichecks "${includes[@]}" checks/main.c checks/target.c "$lib" -o "$b/target"
  "$b/target" | tee "$b/target.log"
  awk '
    $1=="PASS" && $3 ~ /^checks=[1-9][0-9]*$/ {
      if ($2=="DictionaryOrder" || $2=="NestedSlices" || $2=="IndexOverflow" || $2=="Compatibility") seen[$2]++
    }
    $1=="EXECUTED" && $2==4 && $3=="CHECKS" && $4 ~ /^[1-9][0-9]*$/ {summary++}
    END {exit !(seen["DictionaryOrder"]==1 && seen["NestedSlices"]==1 && seen["IndexOverflow"]==1 && seen["Compatibility"]==1 && summary==1)}
  ' "$b/target.log"
  "$b/baseline"
  legacy ;;
esac
