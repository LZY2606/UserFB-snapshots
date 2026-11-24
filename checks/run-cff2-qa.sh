#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case_file=checks/cff2_geometry_qa.cpp
for f in "$case_file" checks/cff2_geometry_model.hpp checks/run-cff2-mutants.sh; do
  test -s "$f" || { printf 'CFF2_TARGET_MISSING %s\n' "$f" >&2; exit 2; }
done
test -s .cff2-check/hb.o || { echo 'run sh checks/install.sh first' >&2; exit 2; }
mkdir -p .cff2-check/output
${CXX:-c++} -std=c++11 -O0 -g0 -DNDEBUG -pthread -Isrc "$case_file" .cff2-check/hb.o -o .cff2-check/qa
for group in model fd-scope partial full; do
  log=".cff2-check/output/$group.log"
  .cff2-check/qa --case "$group" --out-dir .cff2-check/output > "$log" 2>&1 || { cat "$log"; exit 1; }
  cat "$log"
  awk -v c="$group" '
    $1=="CFF2_QA_PASS" && $2==c && $3 ~ /^comparisons=[1-9][0-9]*$/ { n++ }
    END { exit(n==1 ? 0 : 1) }
  ' "$log" || { echo "CFF2_TARGET_NO_EXECUTED_CASE $group" >&2; exit 3; }
done
test -s .cff2-check/output/cff2-outline.ppm
signature=$(head -c 2 .cff2-check/output/cff2-outline.ppm)
test "$signature" = P6
test "$(wc -c < .cff2-check/output/cff2-outline.ppm)" -gt 128
sh checks/run-cff2-mutants.sh
sh checks/baseline.sh
printf '%s\n' CFF2_QA_COMPLETE
