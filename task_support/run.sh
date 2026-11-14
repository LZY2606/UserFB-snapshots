#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/multissrc.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++20 -DNO_OPENSSL=1 -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include task_support/driver.c task_tests/multissrc.cpp .task-build/liveMedia.a .task-build/groupsock.a .task-build/BasicUsageEnvironment.a .task-build/UsageEnvironment.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in isolation loss clock lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++20 -DNO_OPENSSL=1 -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include task_support/baseline.cpp .task-build/liveMedia.a .task-build/groupsock.a .task-build/BasicUsageEnvironment.a .task-build/UsageEnvironment.a -o .task-build/baseline
.task-build/baseline
