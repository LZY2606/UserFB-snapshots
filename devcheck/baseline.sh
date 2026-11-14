#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/jpeg-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
build_case devcheck/baseline.cpp
"$b/case"
