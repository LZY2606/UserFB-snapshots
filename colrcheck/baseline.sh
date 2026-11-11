#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/colr-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I. colrcheck/baseline.c -lm -o "$build/baseline"
"$build/baseline" colrcheck/geometry.ttf
