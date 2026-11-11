#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for file in test/animation/runtime/inertialization_job_tests.cc checks/inertialization_demo.cc; do
  if [[ ! -s "$file" ]]; then echo "TARGET_FILE_MISSING_OR_EMPTY $file"; exit 1; fi
done
bash checks/configure.sh
cmake --build build-check --parallel 4 --target test_inertialization_job inertialization_demo
logdir=$(mktemp -d "${TMPDIR:-/tmp}/ozz-inertial.XXXXXX")
trap 'rm -rf "$logdir"' EXIT
./build-check/checks/test_inertialization_job --gtest_filter='Inertialization*.*' | tee "$logdir/test.log"
bash checks/check-gtest-output.sh "$logdir/test.log"
./build-check/checks/inertialization_demo | tee "$logdir/demo.log"
if ! grep -q '^INERTIALIZATION_DEMO_OK$' "$logdir/demo.log"; then echo TARGET_DEMO_MARKER_MISSING; exit 1; fi
bash checks/run-baseline.sh
echo INERTIALIZATION_TARGET_OK
