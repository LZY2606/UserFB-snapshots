#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
baseline) test_src=support/baseline.cc ;;
regression) test_src=tests/literal_relocation_regression.cc
  extra_src=support/regression_main.cc
  test -s "$test_src" || { echo "missing nonempty $test_src" >&2; exit 64; } ;;
*) exit 64 ;;
esac
mkdir -p .offline-build
c++ -std=c++17 -O1 -DDOBBY_LOGGING_DISABLE -Isource -Isource/dobby -Iinclude -I. -Iexternal -Iexternal/logging \
  source/InstructionRelocation/arm64/InstructionRelocationARM64.cc support/memory_adapter.cc "$test_src" ${extra_src:-} -o .offline-build/runner
.offline-build/runner
