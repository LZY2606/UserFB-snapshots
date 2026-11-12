#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp)
trap 'rm -f "$out"' EXIT
bash support/go.sh test -count=1 -v ./mp4 -run '^(TestEncryptDecrypt|TestDecryptSegmentWithKeys|TestParseSencWithSeparateInit|TestParseSencWithSeig|TestEncryptFragmentNoAuxInfoOmitsBoxes|TestCryptSampleBadSubSamplePatterns)$' >"$out" 2>&1 || { cat "$out"; exit 1; }
cat "$out"
for name in TestEncryptDecrypt TestDecryptSegmentWithKeys TestParseSencWithSeparateInit TestParseSencWithSeig TestEncryptFragmentNoAuxInfoOmitsBoxes TestCryptSampleBadSubSamplePatterns; do
  grep -Eq -- "^--- PASS: $name \\(" "$out" || { echo "no executed PASS for $name" >&2; exit 1; }
done
