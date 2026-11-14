#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_atlas.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O2 -I. -c task_support/library.cpp -o .task-build/library.o
c++ -std=c++11 -O2 -I. task_support/baseline.cpp .task-build/library.o -o .task-build/baseline
c++ -std=c++11 -O2 -I. task_support/colored_baseline.cpp .task-build/library.o -o .task-build/colored-baseline
c++ -std=c++11 -O2 demo/tests.cpp -o .task-build/upstream-tests
c++ -std=c++11 -O2 apps/vox2obj.cpp -o .task-build/vox2obj
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -include stdint.h -include stddef.h -x c++ task_support/driver.c tests/task_atlas.cpp -x none .task-build/library.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry texture ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/colored-baseline
(cd demo/vox && ../../.task-build/upstream-tests)
cli_exit=0
.task-build/vox2obj --mesh_algorithm greedy --frames 0 0 --output_name .task-build/old-cli demo/vox/chr_sword.vox || cli_exit=$?
test "$cli_exit" = 0 || test "$cli_exit" = 4
test -s .task-build/old-cli-000.obj
test -s .task-build/old-cli.mtl
test -s .task-build/old-cli.tga
echo "BASELINE vox2obj PASS obj+mtl+tga exit=$cli_exit"
