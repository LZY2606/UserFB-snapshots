#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if true; then
 log=$(mktemp)
 trap 'rm -f "$log"' EXIT
 set +e
 bash task_support/target_inner.sh "$@" >"$log" 2>&1
 status=$?
 set -e
 cat "$log"
 [ "$status" = 0 ] || exit "$status"
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestOpenDMLSegments([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestOpenDMLSegments checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestOpenDMLIndexes([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestOpenDMLIndexes checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestOpenDMLLimit([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestOpenDMLLimit checks=[1-9][0-9]*$' "$log"

else
 exec bash task_support/target_inner.sh "$@"
fi
