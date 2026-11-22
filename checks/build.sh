#!/bin/bash
set -euo pipefail
mkdir -p .task-build
extra_link=""
if [[ $(uname -s) == Linux ]]; then extra_link="-ldl"; fi
if [[ ${1:-} == target ]]; then
  "${CC:-cc}" -std=gnu99 -O1 -g -I. -Wno-pointer-sign -include checks/paths.h task_support/driver.c checks/resume_contract.c $extra_link -o .task-build/target
  exit
fi
"${CC:-cc}" -std=gnu99 -O1 -g -Wno-pointer-sign -include checks/paths.h afl-fuzz.c $extra_link -o .task-build/afl-fuzz
"${CC:-cc}" -std=gnu99 -O1 -g -I. -Wno-pointer-sign -include checks/paths.h checks/upstream_baseline.c $extra_link -o .task-build/upstream-baseline
