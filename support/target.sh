#!/bin/bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || { d="$TMPDIR/mad-target.$$"; mkdir -p "$d"; echo "$d"; })
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build" target
"$build/run" | tee "$build/result.log"
for group in layout hybrid stream; do
 grep -Eq "^GROUP $group checks=[1-9][0-9]*$" "$build/result.log"
done
grep -Eq "^MAD_MIXED8K_OK checks=[1-9][0-9]*$" "$build/result.log"
bash support/baseline.sh
