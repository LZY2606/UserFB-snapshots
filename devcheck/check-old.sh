#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
mkdir -p build
cc -std=c99 -O1 -I. devcheck/current.c -lm -o build/compound-current
build/compound-current --normal-font
cc -std=c99 -O1 -I. -DTT_TEST tests/test_truetype.c devcheck/old-support.c -lm -o build/upstream-tt-test
build/upstream-tt-test 65 32 build/geometric.ttf >build/upstream-tt-test.log
[[ -s build/upstream-tt-test.log ]]
grep -F 'M@i' build/upstream-tt-test.log >/dev/null
printf 'UPSTREAM_TT_TEST_OK codepoint=65 output_bytes=%s\n' "$(wc -c < build/upstream-tt-test.log | tr -d ' ')"
