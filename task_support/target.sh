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
 if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSpectralSensor([/ ]|$)' "$log"; then exit 4; fi

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSpectralMedia([/ ]|$)' "$log"; then exit 4; fi

if grep -Eq '^[[:space:]]*--- (SKIP|FAIL): TestSpectralTransport([/ ]|$)' "$log"; then exit 4; fi

else
 exec bash task_support/target_inner.sh "$@"
fi
