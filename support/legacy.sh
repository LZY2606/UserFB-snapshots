#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build.sh"
for name in next_fast_size twotonetest test_real; do
  "${CC:-cc}" "${cc_args[@]}" -DNUMFFTS=2 "${sources[@]}" "test/$name.c" -lm -o "$build_dir/$name"
done
"$build_dir/next_fast_size"
"$build_dir/twotonetest" 480
"$build_dir/test_real" 120
"$build_dir/test_real" 514
printf 'BASELINE_SELECTED_UPSTREAM_C_TESTS_OK scalar=%s\n' "${SCALAR:-float}"
