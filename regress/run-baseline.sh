#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d);trap 'rm -rf "$b"' EXIT
cc -std=c99 -Wall -Wextra -I . regress/baseline.c -lm -o "$b/baseline"
"$b/baseline"
cc -std=c99 -Wall -Wextra -c tests/compilation/dr_wav.c -o "$b/old-compilation.o"
echo DRWAV_OLD_COMPILATION_OK
