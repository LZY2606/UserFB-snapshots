#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
work=$(mktemp -d "${TMPDIR:-/tmp}/rectpack2d-baseline.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -pedantic -Isrc tests/atlas/baseline.cpp -o "$work/baseline"
"$work/baseline" < tests/atlas/baseline.in > "$work/result"
cat "$work/result"
diff -u tests/atlas/baseline.expected "$work/result"
