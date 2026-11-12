#!/bin/sh
set -eu
. ./checks/env.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
go build -mod=readonly -o "$b/baseline" ./checks/baseline
"$b/baseline"
go test -mod=readonly -run '^Test(PathLength|FontFace|TextLine)$' -v -count=1 . > "$b/old"
cat "$b/old"
for name in TestPathLength TestFontFace TestTextLine; do grep -F -- "--- PASS: $name " "$b/old" >/dev/null; done
echo CANVAS_OLD_TESTS_OK
