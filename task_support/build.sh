#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
mode=${1:-baseline}
if [ "$mode" = baseline ]; then
  set -- task_support/normal.c task_support/witness.c
elif [ "$mode" = target ]; then
  test -s librtp/test/h264-interleaved-test.c
  set -- task_support/normal.c task_support/target_main.c librtp/test/h264-interleaved-test.c
else exit 2; fi
${CC:-cc} -std=c11 -O1 -g -DNDEBUG -Wall -Wextra -I librtp/include -I librtp/payload librtp/source/rtp-packet.c librtp/payload/rtp-h264*.c "$@" -o ".task-build/$mode"
exec ".task-build/$mode"
