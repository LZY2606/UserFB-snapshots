#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'cmake -E remove_directory "$build"' EXIT
cc -std=c99 -O2 -Wall -Wextra support/baseline.c -lm -o "$build/baseline"
"$build/baseline"
cc -std=c99 -O2 -Wall -Wextra support/legacy.c -lm -o "$build/legacy"
"$build/legacy"
