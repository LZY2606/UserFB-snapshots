#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc -std=c99 -O1 -I. checks/baseline.c ufbx.c -lm -o "$build/baseline"
"$build/baseline"
