#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(pkg-config --modversion opus)" = 1.6.1
build=$(mktemp -d "$PWD/.baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
bash devcheck/build-test.sh devcheck/baseline.c "$build/base"
"$build/base"
bash devcheck/build-test.sh examples/opusenc_example.c "$build/example"
dd if=/dev/zero of="$build/pcm" bs=4 count=4410 2>/dev/null
"$build/example" "$build/pcm" "$build/example.opus"
clang -std=c99 devcheck/check_example.c -o "$build/check"
"$build/check" "$build/example.opus"
printf '%s\n' 'OPUSENC_BASELINE_OK old_example=1'
