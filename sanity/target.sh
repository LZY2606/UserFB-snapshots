#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S sanity -B .build -DCMAKE_BUILD_TYPE=Release -DARAP_TARGET=ON
cmake --build .build --target arap_tests baseline old_tests -j 2
filter='ArapTest.rigid_motion:ArapTest.bending_energy:ArapTest.handle_reuse:ArapTest.disconnected:ArapTest.sparse_handles:ArapTest.rejection'
.build/arap_tests --gtest_color=no --gtest_filter="$filter" --gtest_list_tests > .build/list.log
cat .build/list.log
for name in rigid_motion bending_energy handle_reuse disconnected sparse_handles rejection; do
 grep -Eq "^  ${name}$" .build/list.log || { echo "missing test: $name" >&2; exit 1; }
done
.build/arap_tests --gtest_color=no --gtest_filter="$filter" --gtest_output=xml:.build/arap.xml | tee .build/target.log
grep -Fq '[  PASSED  ] 6 tests.' .build/target.log
! grep -Fq '[  SKIPPED ]' .build/target.log
.build/baseline
.build/old_tests --gtest_color=no --gtest_filter='FairingTest.*' | tee .build/old.log
grep -Fq '[  PASSED  ] 2 tests.' .build/old.log
! grep -Fq '[  SKIPPED ]' .build/old.log
echo PMP_ARAP_TARGET_OK
