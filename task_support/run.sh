#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/trace.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_TESTING=ON -DGLSLOPT_BUILD_TOOLS=OFF -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build/lib --target glsl_optimizer glsl_optimizer_tests -j 4
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -I. -Isrc/glsl -x c++ task_support/driver.c task_tests/trace.cpp -x none .task-build/lib/libglsl_optimizer.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in catalog execution compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++17 -O0 -I. -Isrc/glsl checks/baseline.cpp .task-build/lib/libglsl_optimizer.a -pthread -o .task-build/baseline
.task-build/baseline
c++ -std=c++17 -O0 -I. -Isrc/glsl checks/metal_baseline.cpp .task-build/lib/libglsl_optimizer.a -pthread -o .task-build/metal-baseline
.task-build/metal-baseline
c++ -std=c++17 -O0 -I. -Isrc/glsl checks/postprocess_baseline.cpp .task-build/lib/libglsl_optimizer.a -pthread -o .task-build/postprocess-baseline
.task-build/postprocess-baseline
ctest --test-dir .task-build/lib -R '^test_runner$' --output-on-failure
printf 'BASELINE_COMPLETE\n'
