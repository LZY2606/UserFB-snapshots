#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c17 -O2 -I. -Ichecks checks/repro.c -lm -o "$build/repro"
"$build/repro" "${1:-layers}"
