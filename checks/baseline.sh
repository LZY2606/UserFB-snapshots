#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I. checks/baseline.c -lm -lpthread -o "$b/check"
"$b/check"
