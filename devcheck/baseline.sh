#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/twolame-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
build_case devcheck/baseline.c
"$b/case"
