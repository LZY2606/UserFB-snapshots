#!/usr/bin/env bash
set -euo pipefail
test -s test/vgm_stream_control.cpp
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b/build"
"${CXX:-c++}" -std=c++11 -I. support/baseline.cpp "$b/build/gme/libgme.a" -o "$b/smoke"
"${CXX:-c++}" -std=c++11 -I. test/vgm_stream_control.cpp "$b/build/gme/libgme.a" -o "$b/test"
if "$b/test" --case does-not-exist > "$b/unknown.log" 2>&1; then
  printf 'unknown case must fail\n' >&2; exit 1
fi
for name in bank_blocks rational_waits controls loop_restart invalid legacy; do
  "$b/test" --case "$name" | tee "$b/$name.log"
  grep -Fxq "VGM_STREAM_CASE $name PASS" "$b/$name.log"
done
"$b/smoke" --goal
bash support/baseline.sh normal
printf 'VGM_STREAM_CONTROL_OK cases=6\n'
