#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/async_target.cpp; fi
mkdir -p .task-build
sources=(AudioFFT.cpp FFTConvolver.cpp TwoStageFFTConvolver.cpp Utilities.cpp)
if [[ $mode == target ]]; then
  test -s AsyncFFTConvolver.cpp
  test -s AsyncFFTConvolver.h
  test -s TailJob.cpp
  test -s TailJob.h
  test -s examples/async_tail.cpp
  sources+=(AsyncFFTConvolver.cpp TailJob.cpp)
  c++ -std=c++17 -O2 -pthread -I. -include AsyncFFTConvolver.h -include TailJob.h -c task_support/header_consumer.cpp -o .task-build/header_consumer.o
fi
objects=()
for source in "${sources[@]}"; do
  object=.task-build/$(basename "$source" .cpp).o
  c++ -std=c++17 -O2 -pthread -I. -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libtask.a
ar rcs .task-build/libtask.a "${objects[@]}"
c++ -std=c++17 -O2 -pthread -I. task_support/baseline.cpp .task-build/libtask.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -pthread -I. test/async_target.cpp task_support/driver.c .task-build/header_consumer.o .task-build/AsyncFFTConvolver.o .task-build/TailJob.o .task-build/libtask.a -o .task-build/target
c++ -std=c++17 -O2 -pthread -I. task_support/api_consumer.cpp .task-build/AsyncFFTConvolver.o .task-build/TailJob.o .task-build/libtask.a -o .task-build/api_consumer
.task-build/api_consumer
c++ -std=c++17 -O2 -pthread -I. examples/async_tail.cpp .task-build/AsyncFFTConvolver.o .task-build/TailJob.o .task-build/libtask.a -o .task-build/example
c++ -std=c++17 -O2 task_support/check_pcm.cpp -o .task-build/check_pcm
rm -f .task-build/async-tail.pcm
.task-build/example
test -s .task-build/async-tail.pcm
.task-build/check_pcm .task-build/async-tail.pcm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pcm races lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
