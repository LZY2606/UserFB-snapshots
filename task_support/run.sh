#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/location_at_contract.cc; fi
mkdir -p .task-build
c++ -std=c++11 -O1 -I. -Ielf -Idwarf -Itask_support -c task_support/driver.c -o .task-build/driver.o
for src in elf/*.cc dwarf/*.cc; do
 obj=".task-build/$(dirname "$src")_$(basename "${src%.cc}").o"
 c++ -std=c++11 -O1 -Ielf -Idwarf -c "$src" -o "$obj"
done
ar rcs .task-build/libelfin.a .task-build/elf_*.o .task-build/dwarf_*.o
if [[ $mode == target ]]; then
  c++ -std=c++11 -O1 -I. -Ielf -Idwarf tests/location_at_contract.cc .task-build/driver.o .task-build/libelfin.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in select ownership frame errors; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -O1 -I. -Ielf -Idwarf -Itask_support task_support/baseline.cc .task-build/libelfin.a -o .task-build/baseline
.task-build/baseline
c++ -std=c++11 -O1 -I. -Ielf -Idwarf task_support/expression_baseline.cc .task-build/libelfin.a -o .task-build/expression-baseline
.task-build/expression-baseline
c++ -std=c++11 -O1 -Ielf -Idwarf examples/dump-sections.cc .task-build/libelfin.a -o .task-build/sections
.task-build/sections test/golden-gcc-4.9.2/example > .task-build/sections.out
cmp .task-build/sections.out test/golden-gcc-4.9.2/sections
echo ELF_GOLDEN_SECTIONS_OK
