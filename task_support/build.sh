set -euo pipefail
build_library() {
  local out=$1
  mkdir -p "$out"
  for src in src/*.c; do
    if [[ ${TASK_SPEC_BRIDGE:-0} = 1 && "$src" = src/spec.c ]]; then continue; fi
    "${CC:-cc}" -std=c11 -O2 -Iinclude -Isrc ${TASK_CPPFLAGS:-} -c "$src" -o "$out/$(basename "${src%.c}").o"
  done
}
build_target() {
  local out=$1
  TASK_SPEC_BRIDGE=1 TASK_CPPFLAGS="${TASK_CPPFLAGS:-} -DLC3_ENTROPY_TRACE" build_library "$out"
  "${CC:-cc}" -std=c11 -O2 -DLC3_ENTROPY_TRACE -Iinclude -Isrc -Itask_support -c task_support/target_bridge.c -o "$out/spec.o"
  "${CXX:-c++}" -std=c++17 -O2 -Iinclude -Isrc -Itask_support ${TASK_CPPFLAGS:-} task_support/target.cpp task_support/entropy_reference.cpp task_support/main.cpp "$out"/*.o -lm -o "$out/target"
}
