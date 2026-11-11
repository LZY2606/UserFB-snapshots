#!/bin/sh
set -eu
# Honor $TMPDIR (some sandboxes disallow the OS default temp location).
b=$(mktemp -d "${TMPDIR:-/tmp}/spectral-ducker.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -I. checks/spectral_ducker.cpp -o "$b/check"
"$b/check"
