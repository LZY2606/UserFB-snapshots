#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d); trap 'rm -rf "$build"' EXIT
cc -std=c99 -I. -c spirv_reflect.c -o "$build/reflect.o"
c++ -std=c++14 -I. checks/current.cpp "$build/reflect.o" -o "$build/current"
"$build/current"
