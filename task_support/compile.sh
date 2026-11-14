#!/bin/bash
set -euo pipefail
output=$1; shift
c++ -std=c++11 -O2 -pthread -DENABLE_MONOTONIC_CLOCK=1 -DSRT_ENABLE_APP_READER -DSRT_ENABLE_CLOSE_SYNCH -I. -Isrtcore -Icommon -Ihaicrypt -I.task-build/srt -x c++ "$@" -x none .task-build/srt/libsrt.a -o "$output"
