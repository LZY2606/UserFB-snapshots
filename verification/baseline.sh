#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
source verification/build.sh
cc -std=c99 -O2 -DOPJ_STATIC -Isrc/lib/openjp2 -I"$b/build/src/lib/openjp2" verification/baseline.c "$b/build/bin/libopenjp2.a" -lpthread -lm -o "$b/probe"
"$b/probe"
