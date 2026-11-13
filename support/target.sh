#!/bin/bash
set -euo pipefail
source support/build.sh
compile_target "$R/support/target_cases.c" "$B/target"
"$B/target" | tee "$B/target.log"
for group in normal boundary compat; do grep -Eq "^GROUP $group assertions=[1-9][0-9]*$" "$B/target.log"; done
grep -Eq '^TARGET_COMPLETE assertions=[1-9][0-9]*$' "$B/target.log"
compile "$R/support/baseline.c" "$B/baseline"
"$B/baseline"
