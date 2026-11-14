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
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSourceMapDiscovery([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestSourceMapDiscovery checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSourceMapProvenance([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestSourceMapProvenance checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSourceMapGraph([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestSourceMapGraph checks=[1-9][0-9]*$' "$log"

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSourceMapCompletion([/ ]|$)' "$log"; then exit 4; fi
grep -Eq 'TASK_GROUP_DONE TestSourceMapCompletion checks=[1-9][0-9]*$' "$log"

else
 exec bash task_support/target_inner.sh "$@"
fi
