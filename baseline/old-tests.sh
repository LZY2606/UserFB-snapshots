#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
log=.build/old-tests.log
mkdir -p .build
sh baseline/gtest.sh old > "$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
count=$(grep -c '^\[       OK \]' "$log" || true)
test "$count" -eq 17 || { echo "Expected 17 actual upstream passes, got $count" >&2; exit 1; }
echo LIBTESS_UPSTREAM_17_OK
