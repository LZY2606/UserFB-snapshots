#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
test -s test/body-flow-contract.cjs
node --test --test-reporter=tap test/body-flow-contract.cjs | tee "$b/target.log"
grep -Eq '^ok [0-9]+ - BODY flow contract$' "$b/target.log"
grep -Eq '^# pass [1-9][0-9]*$' "$b/target.log"
grep -Eq '^# (fail|cancelled|skipped) 0$' "$b/target.log"
for m in IMAP_PRESSURE_OK IMAP_DISCARD_OK IMAP_FETCH_OK; do grep -Fq "$m" "$b/target.log"; done
node test/test.js
node support/baseline.cjs
