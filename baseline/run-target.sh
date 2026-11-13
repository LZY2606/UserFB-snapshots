#!/usr/bin/env bash
set -euo pipefail
test -s tests/minimum_phase_test.c
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DBUILD_TESTING=ON -DLIBSAMPLERATE_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$b" --target minimum_phase_test simple_test varispeed_test multi_channel_test -j4
"$b/tests/minimum_phase_test" | tee "$b/new.log"
for name in causal magnitude phase streaming lifecycle; do
 grep -Fx "MINIMUM_PHASE_CASE $name PASS" "$b/new.log"
done
ctest --test-dir "$b" -R '^(simple_test|varispeed_test|multi_channel_test)$' --output-on-failure | tee "$b/old.log"
grep -F '100% tests passed, 0 tests failed out of 3' "$b/old.log"
printf 'MINIMUM_PHASE_OK cases=5\n'
