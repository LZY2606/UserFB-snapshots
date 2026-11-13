#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/dasm/width_flow_cases.c; fi
mkdir -p .task-build
make -s -C src -j2 da65 ca65 ld65 CC=cc >.task-build/build.log 2>&1
if [[ $mode == target ]]; then
  cc -std=c99 -Isrc/common -Isrc/da65 -Dmain=task_da65_main -c src/da65/main.c -o .task-build/main-noentry.o
objects=()
for object in wrk/da65/*.o; do [[ $object == wrk/da65/main.o ]] || objects+=("$object"); done
cc -std=c99 -I. -Isrc/common -Isrc/da65 task_support/driver.c test/dasm/width_flow_cases.c .task-build/main-noentry.o "${objects[@]}" wrk/common/common.a -lm -o .task-build/target
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  set +e
  .task-build/target >"$log" 2>&1
  status=$?
  set -e
  cat "$log"
  [[ $status == 0 ]] || exit "$status"
  for group in modes flow listing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$log"
fi
make -s -B -C test/dasm ../../testwrk/dasm/65816-reass.bin CC=cc
cmp testwrk/dasm/test65816.bin testwrk/dasm/65816-reass.bin
printf 'CC65_65816_ROUNDTRIP_OK
'
cc -std=c99 task_support/make_fixture.c -o .task-build/make_fixture
.task-build/make_fixture
bin/da65 --cpu 65816 --start-addr 0x8000 -o .task-build/plain.s .task-build/state.bin
cat .task-build/plain.s
bin/da65 --cpu 65816 -i task_support/state.info -o .task-build/manual.s .task-build/state.bin
cat .task-build/manual.s
grep -Fq 'lda     #$6056' .task-build/manual.s
grep -Fq '.byte' .task-build/plain.s
printf 'DA65_WIDTH_BASELINE_OK
' 
