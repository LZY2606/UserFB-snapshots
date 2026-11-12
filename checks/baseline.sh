#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-xpath.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O0 -I src src/pugixml.cpp checks/baseline.cpp -o "$b/witness"
"$b/witness"
c++ -std=c++17 -O0 -I src src/pugixml.cpp tests/main.cpp tests/test.cpp tests/allocator.cpp tests/writer_string.cpp tests/test_xpath_paths.cpp tests/test_xpath_api.cpp tests/test_xpath_variables.cpp -o "$b/old"
"$b/old"
