#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
node --test --test-reporter=tap test/animation.test.mjs > "$b/result" 2>&1 || { cat "$b/result"; exit 1; }
cat "$b/result"
grep -Eq '^ok [0-9]+ - ordered-overrides$' "$b/result"
grep -Eq '^ok [0-9]+ - overlap-accel$' "$b/result"
grep -Eq '^ok [0-9]+ - reset-slices$' "$b/result"
grep -Eq '^ok [0-9]+ - color-alpha$' "$b/result"
grep -Eq '^ok [0-9]+ - time-domain$' "$b/result"
grep -Eq '^ok [0-9]+ - compatibility$' "$b/result"
grep -Fxq "# tests 6" "$b/result"
grep -Fxq "# pass 6" "$b/result"
grep -Fxq "# skipped 0" "$b/result"
echo TARGET_ALL_PASS
