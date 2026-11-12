#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c17 -O2 -I. -Ichecks checks/baseline.c -lm -o "$build/baseline"
"$build/baseline"
