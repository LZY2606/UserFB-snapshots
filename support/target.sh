#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s tests/target_cases.c || { echo 'missing tests/target_cases.c' >&2; exit 2; }
sh support/build.sh tests/target_cases.c support/main.c
.build/check
