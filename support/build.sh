#!/usr/bin/env bash
set -euo pipefail
cc -std=c99 -O2 -Ilib/include -Isupport lib/source/aes*.c lib/source/ctr_mode.c lib/source/utils.c "$@"
