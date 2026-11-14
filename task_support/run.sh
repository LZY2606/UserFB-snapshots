#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/trailers.c; fi
bash task_support/build.sh
mkdir -p .task-build
if [[ $mode == target ]]; then
  cc -std=c99 -DHAVE_CONFIG_H -DHTS_INTERNAL_BUILD -DHTS_INTERNAL_BYTECODE -I. -Isrc -I.build/engine -I.build/deps/include task_support/driver.c task_tests/trailers.c .build/engine/src/.libs/libhttrack.a .build/deps/lib/libz.a -lpthread -ldl -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  node task_support/with-server.cjs .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in network spool cache; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then node task_support/baseline.cjs; fi
