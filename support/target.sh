#!/bin/bash
set -euo pipefail
# This runner compiles the prescribed translation unit itself; no user shell runner.
test -s support/target_cases.cpp
source support/build.sh
compile_target "$R/support/target_cases.cpp" "$B/target"
"$B/target" > "$B/target.stdout"
cat "$B/target.stdout"
# Early exit(0), a skipped group or an empty task_cases must not succeed.
for group in normal boundary compat; do
 if [[ $(grep -Ec "^GROUP ${group} assertions=[1-9][0-9]*$" "$B/target.stdout") != 1 ]]; then
  echo "MISSING_EXECUTED_GROUP: $group" >&2; exit 20
 fi
done
if [[ $(grep -Ec '^TARGET_COMPLETE assertions=[1-9][0-9]*$' "$B/target.stdout") != 1 ]] ||
   [[ $(tail -n 1 "$B/target.stdout") != TARGET_COMPLETE\ assertions=* ]]; then
 echo 'MISSING_TARGET_COMPLETION' >&2; exit 21
fi
compile "$R/support/baseline.cpp" "$B/baseline"
"$B/baseline"
source support/old-tests.sh
