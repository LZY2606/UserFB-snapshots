#!/bin/bash
set -euo pipefail
mode=${1:-}
case "$mode" in baseline) flag=OFF ;; target)
  test -s gme/Nsf_Pump.h
  test -s gme/Nsf_Pump.cpp
  flag=ON ;;
  *) echo 'usage: bash task_support/run.sh baseline|target' >&2;exit 2;;
esac
mkdir -p .task-build
cmake -S task_support -B .task-build/production -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET="$flag" >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log;exit 1; }
cmake --build .task-build/production --clean-first -j 2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log;exit 1; }
if [[ $mode == target ]];then
  .task-build/production/api_consumer >.task-build/api-consumer.log 2>&1 || { cat .task-build/api-consumer.log;exit 1; }
  cat .task-build/api-consumer.log
  grep -q '^NSF_PUMP_API_OK pcm_equal=1 bounded_work=1 lifecycle=1$' .task-build/api-consumer.log
fi
.task-build/production/baseline
