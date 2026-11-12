#!/bin/sh
set -eu
test -s test/collection-contract.mjs
build=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$build"; rm -f "$log"' EXIT
./node_modules/.bin/tsc --ignoreDeprecations 6.0 --outDir "$build"
if ! CHECK_BUILD_DIR="$build" NODE_PATH="$PWD/node_modules" node --test --test-reporter=tap test/collection-contract.mjs >"$log" 2>&1; then cat "$log"; exit 1; fi
cat "$log"
for name in RDFA_DIRECTION RDFA_LIST_SCOPE RDFA_LIST_MULTIPLICITY RDFA_SCOPE_TRANSFORMS; do
 grep -Eq "^ok [0-9]+ - $name$" "$log"
done
grep -Eq '^# tests ([4-9]|[1-9][0-9]+)$' "$log"
grep -q '^# fail 0$' "$log"
grep -q '^# skipped 0$' "$log"
grep -q '^# cancelled 0$' "$log"
echo TARGET_RDFA_CONTRACT_OK
