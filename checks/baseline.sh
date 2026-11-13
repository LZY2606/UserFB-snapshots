#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
.check/baseline/check_baseline
ctest --test-dir .check/old --output-on-failure -R '^test_(sample_ping_pong|sample_offset|sample_offset_beyond_loop|retrigger_effect_sample_change|pitch_slides_s3m)$' | tee .check/old/baseline-ctest.stdout
grep -Fq '100% tests passed, 0 tests failed out of 5' .check/old/baseline-ctest.stdout
printf '%s\n' 'CHECK_OLD five_tests_passed'
