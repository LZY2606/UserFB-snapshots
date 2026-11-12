#!/bin/bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || { d="$TMPDIR/mad-baseline.$$"; mkdir -p "$d"; echo "$d"; })
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build"
"$build/run"
