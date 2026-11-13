#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh support/build.sh support/baseline.c
.build/check
