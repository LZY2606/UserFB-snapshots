#!/bin/bash
set -euo pipefail
build_case() {
 cc -std=c99 -O2 -D_ERROR_PRINT -Wno-pointer-to-int-cast -D_POSIX_C_SOURCE=200112L -DH264E_MAX_THREADS=1 -I. -Idevcheck/vendor/h264bsd devcheck/encoder.c devcheck/vendor/h264bsd/*.c "$@" -lm -o "$b/case"
}
