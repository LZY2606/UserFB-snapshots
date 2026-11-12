#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build.sh"
test -s support/prime_cases.c || { echo 'MISSING support/prime_cases.c' >&2; exit 4; }
"${CC:-cc}" "${cc_args[@]}" "${sources[@]}" support/driver.c support/prime_cases.c -lm -o "$build_dir/prime-cases"
"$build_dir/prime-cases" > "$build_dir/target.log" 2>&1 || { cat "$build_dir/target.log"; exit 5; }
cat "$build_dir/target.log"
for group in numeric planning workspace consumers; do
  grep -Eq "^GROUP $group assertions=[1-9][0-9]*$" "$build_dir/target.log" || exit 6
done
grep -Eq '^PRIME_TESTS_COMPLETE assertions=[1-9][0-9]*$' "$build_dir/target.log" || exit 7
bash support/legacy.sh
