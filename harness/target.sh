#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -Iinclude -Isrc -Iharness src/simdjson.cpp harness/main.c tests/ondemand/batch_pointer_contract.cpp -pthread -o "$build/target"
"$build/target"
bash harness/baseline.sh
