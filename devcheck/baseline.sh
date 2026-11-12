#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/h264-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
build_case devcheck/baseline.c
"$b/case"
