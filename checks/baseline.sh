#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/spvm-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I inc -I checks src/*.c src/ext/GLSL450.c checks/old_shader.c -lm -o "$b/old"
"$b/old"
cc -std=c99 -I inc -I checks src/*.c src/ext/GLSL450.c checks/continuation_baseline.c -lm -o "$b/continuation"
"$b/continuation"
printf 'BASELINE_OK\n'
