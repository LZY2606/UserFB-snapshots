#!/usr/bin/env bash
set -euo pipefail
test -s checks/target.cpp || { echo 'missing checks/target.cpp' >&2; exit 1; }
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-xpath-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
for mode in narrow wide; do
 flags=""
 if [[ "$mode" == wide ]]; then flags="-DPUGIXML_WCHAR_MODE"; fi
 c++ -std=c++17 -O0 $flags -I src -I checks src/pugixml.cpp checks/target_main.cpp checks/target.cpp -o "$b/target"
 "$b/target"
done
bash checks/baseline.sh
