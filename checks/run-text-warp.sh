#!/bin/sh
set -eu
. ./checks/env.sh
test -s checks/text_warp_test.go
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export CANVAS_WARP_OUT="$b"
go test -mod=readonly -run '^TestTextWarp$' -v -count=1 ./checks > "$b/out"
cat "$b/out"
for name in straight arc_reference curved_outline shaping_and_decorations transformed_tolerance rejected_input; do
 grep -F -- "--- PASS: TestTextWarp/$name " "$b/out" >/dev/null
done
test -s "$b/text-warp.svg"
echo CANVAS_TEXT_WARP_OK
