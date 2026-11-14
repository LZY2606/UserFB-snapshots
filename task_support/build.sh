#!/bin/bash
set -euo pipefail
out=$1;shift
cc -std=gnu99 -O2 -I. -Isrc -Itask_support src/*.c "$@" -o "$out"
