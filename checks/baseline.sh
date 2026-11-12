#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d -p "${TMPDIR:-/tmp}")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -I. checks/baseline.c -lm -o "$b/baseline"
javac -d "$b" Sonic.java checks/Baseline.java
"$b/baseline" "$@"
java -cp "$b" Baseline "$@"
cc -std=c99 -O2 -I. -Itests tests/runtests.c tests/genwave.c sonic.c tests/input_clamping_test.c tests/sonic_api_test.c -lm -o "$b/legacy"
"$b/legacy"
echo SONIC_INPUT_CLAMPING_OK
