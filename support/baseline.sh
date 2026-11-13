#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh support/baseline.c -o "$b/baseline"
"$b/baseline"
