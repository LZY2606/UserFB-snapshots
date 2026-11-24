#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
objects=()
# Production paths and checked-in generated tables: no regeneration is required.
for src in cs.c Mapping.c MCInst.c MCInstrDesc.c MCRegisterInfo.c SStream.c utils.c arch/ARM/*.c; do
 obj=.task-build/obj/${src//\//_}.o
 if [[ ! -f $obj || $src -nt $obj ]] || find include arch/ARM -type f \( -name '*.h' -o -name '*.inc' \) -newer "$obj" -print -quit 2>/dev/null | grep -q . || find . -maxdepth 1 -name '*.h' -newer "$obj" -print -quit 2>/dev/null | grep -q .; then
  cc -std=c11 -O0 -DCAPSTONE_HAS_ARM -DCAPSTONE_USE_SYS_DYN_MEM -Iinclude -I. -c "$src" -o "$obj"
 fi
 objects+=("$obj")
done
rm -f .task-build/libcapstone.a
ar rcs .task-build/libcapstone.a "${objects[@]}"
cc -std=c11 -Iinclude -I. task_support/baseline.c .task-build/libcapstone.a -o .task-build/baseline
cc -std=c11 -Iinclude -I. tests/test_arm.c .task-build/libcapstone.a -o .task-build/test_arm
