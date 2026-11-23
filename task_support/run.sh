#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/task_pcm.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/re -DLIBRE_BUILD_SHARED=OFF -DUSE_OPENSSL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_Backtrace=TRUE -DUSE_SIP=OFF -DUSE_RTMP=OFF -DUSE_PCP=OFF -DUSE_BFCP=OFF >.task-build/config.log 2>&1
cmake --build .task-build/re --target re -j2 >.task-build/build.log 2>&1
cc -std=c11 -D_DEFAULT_SOURCE -I. -Iinclude -I.task-build/re/include task_support/baseline.c .task-build/re/libre.a -lm -lpthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/re_pcm.h
test -s src/rtp/pcm.c
test -s .task-build/re/CMakeFiles/re-objs.dir/src/rtp/pcm.c.o
test -s examples/pcm_replay.c
cc -std=c11 -D_DEFAULT_SOURCE -I. -Iinclude -I.task-build/re/include -include include/re_pcm.h task_support/api_probe.c .task-build/re/libre.a -lm -lpthread -o .task-build/api_probe
.task-build/api_probe
cc -std=c11 -D_DEFAULT_SOURCE -I. -Iinclude -I.task-build/re/include examples/pcm_replay.c .task-build/re/libre.a -lm -lpthread -o .task-build/pcm_replay
rm -f .task-build/pcm_replay.out
.task-build/pcm_replay >.task-build/pcm_replay.out
test -s .task-build/pcm_replay.out
cc -std=c11 -D_DEFAULT_SOURCE -I. -Iinclude -I.task-build/re/include test/task_pcm.c task_support/driver.c .task-build/re/libre.a -lm -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline ingress epochs; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
