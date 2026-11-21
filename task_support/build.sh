#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/core
sources=(src/y262/*.c)
for src in "${sources[@]}"; do
  case "$src" in *_arm64.c) continue ;; esac
  cc -std=c99 -O1 -fno-strict-aliasing -DHAVE_LIBPTHREAD -Isrc/y262 -c "$src" -o ".task-build/core/$(basename "${src%.c}").o"
done
