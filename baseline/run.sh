#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c11 -O0 -Wno-unknown-pragmas -DBDDISASM_HAS_VSNPRINTF -DBDDISASM_HAS_MEMSET -Iinc -Ibddisasm/include bddisasm/*.c bdshemu/*.c baseline/witness.c -o "$build/witness"
"$build/witness"
