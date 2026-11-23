#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/root_blend.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s Horde3D/Source/Horde3DEngine/egRootBlend.h
test -s Horde3D/Source/Horde3DEngine/egRootBlend.cpp
test -s examples/root_blend.cpp
objects=$(find .task-build/CMakeFiles/Horde3D.dir -name '*.o' -type f -print)
test -n "$objects"
c++ -std=c++14 -DH3D_STATIC_LIBS -DCMAKE -I. -IHorde3D/Source/Horde3DEngine -IHorde3D/Source/Shared -IHorde3D/Bindings/C++ -I.task-build -include Horde3D/Source/Horde3DEngine/egRootBlend.h tests/root_blend.cpp task_support/driver.c $objects -o .task-build/target
c++ -std=c++14 -DH3D_STATIC_LIBS -DCMAKE -I. -IHorde3D/Source/Horde3DEngine -IHorde3D/Source/Shared -IHorde3D/Bindings/C++ -I.task-build -include Horde3D/Source/Horde3DEngine/egRootBlend.h examples/root_blend.cpp $objects -o .task-build/root_blend_example
.task-build/root_blend_example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in twist integration pose lifecycle invalid; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/motion_baseline
