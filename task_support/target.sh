#!/usr/bin/env bash
set -euo pipefail
TASK_BUILD=$(mktemp -d);export TASK_BUILD
trap 'rm -rf "$TASK_BUILD"' EXIT
if ! test -s task_tests/target.cpp; then
    echo 'FAIL: task_tests/target.cpp missing or empty' >&2
    exit 1
fi
bash task_support/build.sh
# Root is needed only for quoted task_support/... includes, never for <format>.
c++ -std=c++17 -O1 -iquote . -Iinc task_support/main.cpp task_tests/target.cpp "$TASK_BUILD/cmake/librlottie.a" -pthread -o "$TASK_BUILD/target" > "$TASK_BUILD/build.log" 2>&1 || { cat "$TASK_BUILD/build.log"; exit 1; }
set +e
"$TASK_BUILD/target" > "$TASK_BUILD/run.log" 2>&1
task_test_rc=$?
set -e
cat "$TASK_BUILD/run.log"
printf 'TARGET_PROCESS_EXIT %s\n' "$task_test_rc"
if test "$task_test_rc" -ne 0; then exit "$task_test_rc"; fi
for group in modes isolation matte reuse; do
    if ! grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$TASK_BUILD/run.log"; then
        echo "FAIL: no completed nonzero group $group" >&2
        exit 1
    fi
done
if ! grep -qx TASK_SUITE_COMPLETE "$TASK_BUILD/run.log"; then
    echo 'FAIL: target suite did not complete' >&2
    exit 1
fi
bash task_support/baseline.sh
