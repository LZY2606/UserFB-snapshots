#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s mp4/seig_rotation_test.go || { echo 'missing or empty mp4/seig_rotation_test.go' >&2; exit 1; }
out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash support/go.sh test -count=1 -v ./mp4 -run '^TestSeig(CencRotation|CbcsRotation|DecodePaths|AtomicFailure)$' >"$out" 2>&1 || { cat "$out"; exit 1; }
cat "$out"
if grep -q -- '--- SKIP:' "$out"; then echo 'rotation tests must not skip' >&2; exit 1; fi
for name in TestSeigCencRotation TestSeigCbcsRotation TestSeigDecodePaths TestSeigAtomicFailure; do
  grep -Eq -- "^--- PASS: $name \\(" "$out" || { echo "no executed PASS for $name" >&2; exit 1; }
done
bash support/old-tests.sh
