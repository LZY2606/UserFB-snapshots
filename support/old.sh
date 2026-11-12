#!/usr/bin/env bash
set -euo pipefail
oldlog=$(mktemp)
trap 'rm -f "$oldlog"' EXIT
go test -mod=vendor -v -count=1 -run '^(TestRepeatMatch|TestSequenceMatch|TestSpatialMatchQwerty|TestDictionaryMatch)$' ./matching > "$oldlog"
cat "$oldlog"
for name in TestRepeatMatch TestSequenceMatch TestSpatialMatchQwerty TestDictionaryMatch; do grep -q -- "--- PASS: $name " "$oldlog"; done
echo ZXCVBN_OLD_ASCII_FOUR_PASS
