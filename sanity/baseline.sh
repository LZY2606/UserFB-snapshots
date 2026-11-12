#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S sanity -B .build -DCMAKE_BUILD_TYPE=Release -DARAP_TARGET=OFF
cmake --build .build --target baseline old_tests -j 2
.build/baseline
.build/old_tests --gtest_color=no --gtest_filter='FairingTest.*' | tee .build/old.log
grep -Fq '[  PASSED  ] 2 tests.' .build/old.log
! grep -Fq '[  SKIPPED ]' .build/old.log
