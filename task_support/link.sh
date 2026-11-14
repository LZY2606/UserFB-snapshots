#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
extra=(-pthread -lm)
case "$(uname -s)" in Darwin) extra+=(-framework CoreFoundation);; Linux) extra+=(-ldl);; esac
c++ -std=c++14 -DAL_LIBTYPE_STATIC -I. -Iinclude "$@" .task-build/lib/libopenal.a "${extra[@]}"
