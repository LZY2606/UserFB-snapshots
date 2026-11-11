#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s Tests/boundary_sources_test.cc || { echo 'Missing nonempty Tests/boundary_sources_test.cc' >&2; exit 1; }
test -s Example/boundary_sources.c || { echo "Missing nonempty Example/boundary_sources.c" >&2; exit 1; }
mkdir -p .build
log=.build/target.log
sh baseline/gtest.sh target > "$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
for name in RegularAndCrossing CoincidentOwnership PartialOverlap WindingAndReverse ReuseAndLegacy AllocationFailure; do
 grep -Eq "^\[       OK \] BoundarySources\.$name \([0-9]+ ms\)$" "$log" || { echo "Missing executed PASS: BoundarySources.$name" >&2; exit 1; }
done
cc -std=c99 -O2 -IInclude Example/boundary_sources.c .build/libtess2.a -lm -o .build/boundary-example
.build/boundary-example > .build/example.log
cat .build/example.log
grep -qx BOUNDARY_SOURCE_EXAMPLE_OK .build/example.log
sh baseline/run.sh
sh baseline/old-tests.sh
echo LIBTESS_BOUNDARY_SOURCES_OK
