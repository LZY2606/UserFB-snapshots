#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) test_src=support/baseline.c; extra_src= ;;
 target) test_src=tests/code_graph_cases.c; extra_src=support/main.c
 test -s "$test_src" || { echo "missing nonempty $test_src" >&2; exit 64; } ;;
 *) exit 64 ;;
esac
mkdir -p .offline-build
cc -std=c11 -O1 -DBDDISASM_HAS_VSNPRINTF -DBDDISASM_HAS_MEMSET -I. -Iinc -Ibddisasm/include -Ibdshemu/include \
 bddisasm/*.c bdshemu/*.c "$test_src" $extra_src -o .offline-build/run
.offline-build/run
