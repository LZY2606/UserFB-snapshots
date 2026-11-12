#!/bin/bash
set -euo pipefail
build="$(mktemp -d 2>/dev/null || mktemp -d -t moog-baseline 2>/dev/null || true)"
if [ -z "$build" ]; then
    # Restricted shells can deny mktemp's default user-temp directory; fall
    # back to a gitignored repo-local directory without changing the checks.
    build="build/baseline-checks"
    mkdir -p "$build"
fi
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O2 -ffp-contract=off -Isrc checks/baseline.cpp -o "$build/baseline"
"$build/baseline"
