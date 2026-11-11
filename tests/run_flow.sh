#!/bin/bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/flow.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O0 -Iinclude src/*.c tests/flow_cases.c -o "$build/flow_cases"
"$build/flow_cases"
echo FLOW_CASES_OK
