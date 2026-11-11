#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
for src in Source/*.c; do
    cc -std=c99 -O2 -g -Wall -Wextra -IInclude -c "$src" -o ".build/$(basename "${src%.c}").o"
done
ar rcs .build/libtess2.a .build/*.o
