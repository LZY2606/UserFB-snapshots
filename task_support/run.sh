#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/tfra_seek_cases.cpp; fi
cmake -S . -B .task-build/ap4 -DBUILD_APPS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/ap4 --target ap4 -j 4
${CXX:-c++} -std=c++11 -O2 -I. -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData task_support/baseline.cpp .task-build/ap4/libap4.a -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O2 -I. -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData -x c++ task_support/driver.c task_support/tfra_seek_cases.cpp -x none .task-build/ap4/libap4.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in anchors gating compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
