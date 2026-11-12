#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -x .build/shineenc
test -s js/test/lib/encode.wav
"${CC:-cc}" -std=c99 -O2 devcheck/decode.c -lm -o .build/decode
.build/shineenc -q js/test/lib/encode.wav .build/legacy.mp3
.build/decode .build/legacy.mp3
