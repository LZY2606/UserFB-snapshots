#!/bin/sh
set -eu
mkdir -p .build
find src/lib src/common -type f -name '*.c' | sort > .build/sources
cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -DSTATICALLY_LINKED=0 -Iinclude -Isrc -Isrc/lib -Isrc/common -Isupport $(cat .build/sources) support/gitsha1.c "$@" -lpthread -lm -ldl -o .build/check
