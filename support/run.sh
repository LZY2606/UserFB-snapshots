#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
node support/assemble.cjs "$build/jsfeat.cjs"
export JSFEAT_MODULE="$build/jsfeat.cjs"
node support/baseline.cjs
if [[ "${1:-baseline}" == target ]]; then
  node support/runner.cjs | tee "$build/target.log"
  for group in photometric masks compatibility; do
    grep -Eq "^PASS $group/.+" "$build/target.log"
  done
  grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/target.log"
fi
