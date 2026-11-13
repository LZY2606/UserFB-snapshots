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
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestLocationWriteAliases([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestLocationWriteAliases checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestLocationWritePreflight([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestLocationWritePreflight checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestLocationWriteCompatibility([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestLocationWriteCompatibility checks=[1-9][0-9]*$' "$log"

else
 exec bash task_support/run_inner.sh "$@"
fi
