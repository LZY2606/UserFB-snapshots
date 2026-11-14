#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "${1:-baseline}" = target ]; then
 log=$(mktemp)
 trap 'rm -f "$log"' EXIT
 set +e
 bash task_support/run_inner.sh "$@" >"$log" 2>&1
 status=$?
 set -e
 cat "$log"
 [ "$status" = 0 ] || exit "$status"
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestWARCRecords([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestWARCRecords checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestWARCDedup([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestWARCDedup checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestWARCFailure([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestWARCFailure checks=[1-9][0-9]*$' "$log"

else
 exec bash task_support/run_inner.sh "$@"
fi
