#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/stereo_samples.c || { echo 'MISSING_TARGET tests/stereo_samples.c' >&2;exit 2; }
for ramp in ON OFF; do
  build=".check/target-$ramp"
  cmake -S checks -B "$build" -DCMAKE_BUILD_TYPE=Debug -DXM_RAMPING="$ramp"
  cmake --build "$build" --target check_target -j 4
  mkdir -p "$build"
  set +e
  "$build/check_target" > "$build/target.stdout" 2> "$build/target.stderr"
  status=$?
  set -e
  cat "$build/target.stdout";cat "$build/target.stderr" >&2
  test "$status" -eq 0 || exit "$status"
  for group in load play ramp formats; do
    test "$(grep -Ec "^CHECK_GROUP $group checks=[1-9][0-9]* DONE$" "$build/target.stdout")" -eq 1 || { echo "INCOMPLETE_GROUP $group" >&2;exit 3; }
  done
  test "$(grep -Fxc 'CHECK_TARGET groups=4 DONE' "$build/target.stdout")" -eq 1 || exit 3
  echo "CHECK_MODE ramp=$ramp DONE"
done
cmake -S tests -B .check/old -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_STANDARD=23 -DXM_VERBOSE=OFF
cmake --build .check/old -j 4
ctest --test-dir .check/old --output-on-failure -E '^XXX_' | tee .check/old/target-ctest.stdout
grep -Fq '100% tests passed, 0 tests failed out of 39' .check/old/target-ctest.stdout
echo 'TARGET_COMPLETE'
