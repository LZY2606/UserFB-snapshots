#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
bash support/build.sh support/baseline.cpp -o "$b/baseline"
"$b/baseline"
