#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
mode=${1:-baseline}
if [ "$mode" = baseline ]; then set -- task_support/normal.c task_support/witness.c
elif [ "$mode" = target ]; then test -s librtmp/tests/queued-write-test.c; set -- task_support/normal.c task_support/target_main.c librtmp/tests/queued-write-test.c
else exit 2; fi
${CC:-cc} -std=gnu11 -O1 -g -DNO_CRYPTO '-DRTMPDUMP_VERSION="offline-seed"' -I librtmp librtmp/*.c "$@" -o ".task-build/$mode"
exec ".task-build/$mode"
