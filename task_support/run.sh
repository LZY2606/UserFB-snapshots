#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/pcmu_mix.cpp; fi
mkdir -p .task-build
cmake -S task_support -B .task-build/lib >.task-build/config.log 2>&1
cmake --build .task-build/lib -j2 >.task-build/build.log 2>&1
c++ -std=c++11 -include cstring -include cstdlib -include unistd.h -I. -Isrc -Itask_support/platform task_support/baseline.cpp .task-build/lib/libjrtp.a -lpthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/rtppcmumixer.h
test -s src/rtppcmumixer.cpp
test -s examples/pcmu_mix.cpp
c++ -std=c++11 -include cstring -include cstdlib -include unistd.h -I. -Isrc -Itask_support/platform -include src/rtppcmumixer.h -c src/rtppcmumixer.cpp -o .task-build/required_mixer.o
c++ -std=c++11 -include cstring -include cstdlib -include unistd.h -I. -Isrc -Itask_support/platform -include src/rtppcmumixer.h task_support/api_probe.cpp .task-build/required_mixer.o .task-build/lib/libjrtp.a -lpthread -o .task-build/api_probe
.task-build/api_probe
c++ -std=c++11 -include cstring -include cstdlib -include unistd.h -I. -Isrc -Itask_support/platform examples/pcmu_mix.cpp .task-build/required_mixer.o .task-build/lib/libjrtp.a -lpthread -o .task-build/pcmu_mix
rm -f .task-build/pcmu_mix.out
.task-build/pcmu_mix >.task-build/pcmu_mix.out
test -s .task-build/pcmu_mix.out
c++ -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -include cstring -include cstdlib -include unistd.h -I. -Isrc -Itask_support/platform task_tests/pcmu_mix.cpp .task-build/driver.o .task-build/required_mixer.o .task-build/lib/libjrtp.a -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in slots wire lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
