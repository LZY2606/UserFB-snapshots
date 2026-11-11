#!/usr/bin/env bash
set -euo pipefail
test -s test/harness/null-prototype.test.cjs
out=$(mktemp); trap 'rm -f "$out"' EXIT
node --test --test-reporter=tap test/harness/null-prototype.test.cjs | tee "$out"
for name in provenance alias-invalidation control-flow isolation; do grep -Eq "^ok [0-9]+ - $name$" "$out"; done
grep -Eq '^# tests [1-9][0-9]*$' "$out"
grep -Eq '^# skipped 0$' "$out"
baseline/node_modules/.bin/mocha test/rules/detect-object-injection.js --reporter dot
