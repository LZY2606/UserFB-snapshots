#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) test_src=support/baseline.c; extra_src= ;;
 target) test_src=tests/c/delay_graph_cases.c; extra_src=support/main.c
 test -s "$test_src" || { echo "missing nonempty $test_src" >&2; exit 64; } ;;
 *) exit 64 ;;
esac
mkdir -p .offline-build
find src -name '*.c' -print > .offline-build/sources.list
cc -std=c11 -O1 -I. -Iinclude -Itables $(cat .offline-build/sources.list) "$test_src" $extra_src -o .offline-build/run
.offline-build/run
