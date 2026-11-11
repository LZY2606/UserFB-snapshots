#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh baseline/build.sh
cc -std=c99 -O2 -IInclude baseline/baseline.c .build/libtess2.a -lm -o .build/baseline
.build/baseline
