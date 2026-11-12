#!/bin/bash
set -euo pipefail
# Sourced by the fixed runners; all objects live in their temporary directory.
lib_sources=(libtwolame/*.c)
incs=(-Idevcheck -Ilibtwolame)
build_case() { cc -std=c99 -O2 "${incs[@]}" "${lib_sources[@]}" "$@" -lm -o "$b/case"; }
