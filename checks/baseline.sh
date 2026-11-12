#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c17 -O2 -I. checks/baseline.c emu2413.c -lm -o "$build/baseline"
"$build/baseline"
