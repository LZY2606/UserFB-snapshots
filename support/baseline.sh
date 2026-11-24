#!/bin/bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || echo ".task-build/baseline-fallback.$$")
mkdir -p "$build"
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build"
"$build/run"
