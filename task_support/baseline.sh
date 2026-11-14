#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
cc -std=c99 -O2 -DTEST_VORBIS -I. tests/test_vorbis.c -lm -o .task-build/upstream-vorbis
(cd .task-build && ./upstream-vorbis tiny.ogg)
test "$(wc -c < .task-build/decoded.raw | tr -d ' ')" = 512
echo UPSTREAM_DECODE_COMPLETE
