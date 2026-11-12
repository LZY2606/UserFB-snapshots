#!/bin/bash
set -euo pipefail
# Caller supplies b, a new temporary build tree.
tar --exclude=.git --exclude=verification -cf - . | tar -xf - -C "$b"
make -s -C "$b" -j4 USE_ASM=No BUILDTYPE=Release libopenh264.a > "$b/build.log" 2>&1 || { tail -n 35 "$b/build.log"; exit 1; }
