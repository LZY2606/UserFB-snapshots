#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I. checks/baseline.c -lm -lpthread -o "$b/check"
"$b/check"
