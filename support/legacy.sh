#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
bash support/build.sh src/smallvcm.cxx -o "$b/smallvcm"
"$b/smallvcm" -s 0 -a bpt -i 1 -o "$b/legacy.bmp" > "$b/legacy.log" 2>&1
test -s "$b/legacy.bmp"
printf '%s\n' 'SMALLVCM_LEGACY_BPT_OK'
