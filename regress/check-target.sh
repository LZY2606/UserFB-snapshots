#!/usr/bin/env bash
set -euo pipefail
test -s regress/target.cpp || { echo 'missing regress/target.cpp' >&2; exit 1; }
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-xsd-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
bash regress/build.sh "$b"
c++ -std=c++17 -I include -I "$b" -I regress regress/target.cpp regress/target_main.cpp "$b/libxml2.a" -lm -pthread -o "$b/target"
"$b/target"
cc -std=c99 -I include -I "$b" regress/baseline.c "$b/libxml2.a" -lm -pthread -o "$b/witness"
"$b/witness"
