#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
bash support/build.sh support/driver.cpp tests/thinlens_transport.cpp -o "$b/contract"
set +e
"$b/contract" > "$b/out" 2> "$b/err"
status=$?
set -e
cat "$b/out"
cat "$b/err" >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
awk -f support/verify_output.awk "$b/out"
bash support/baseline.sh
bash support/legacy.sh
