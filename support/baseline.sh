#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh support/baseline.c -o "$build/baseline"
"$build/baseline"
