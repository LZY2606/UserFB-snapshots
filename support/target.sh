#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s tests/target_cases.cpp || { echo 'missing tests/target_cases.cpp' >&2; exit 2; }
mkdir -p .build
sh support/build.sh tests/target_cases.cpp support/main.cpp
cp .build/check .build/target
.build/target
