#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/nsf2_irq_task.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DGME_BUILD_SHARED=OFF -DGME_BUILD_STATIC=ON -DGME_ZLIB=OFF -DGME_BUILD_TESTING=OFF -DGME_BUILD_EXAMPLES=OFF -DUSE_GME_NSF=ON -DUSE_GME_AY=OFF -DUSE_GME_GBS=OFF -DUSE_GME_GYM=OFF -DUSE_GME_HES=OFF -DUSE_GME_KSS=OFF -DUSE_GME_NSFE=OFF -DUSE_GME_SAP=OFF -DUSE_GME_SPC=OFF -DUSE_GME_VGM=OFF > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/cmake --target gme_static -j2 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O2 -I. -Igme -Itask_support -x c++ task_support/driver.c test/nsf2_irq_task.cpp -x none .task-build/cmake/gme/libgme.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timer cpu audio; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 ${CXX:-c++} -std=c++11 -O2 -Igme -Itask_support task_support/baseline.cpp .task-build/cmake/gme/libgme.a -o .task-build/baseline
 .task-build/baseline
fi
