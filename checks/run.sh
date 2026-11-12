#!/bin/bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-build.XXXXXX")
trap 'rm -rf "$b"' EXIT
for f in spiro.c bezctx.c spiroentrypoints.c; do o="$b/$(basename "$f").o"; cc -std=c99 -O2 -I. -Ichecks/compat -c "$f" -o "$o"; done
objects=("$b"/*.o)
if [[ "${1:-target}" == baseline ]]; then
 c++ -std=c++17 -O2 -I. -Ichecks/compat checks/baseline.cpp "${objects[@]}" -lm -o "$b/base"
 "$b/base"
else
 c++ -std=c++17 -O2 -I. -Ichecks/compat checks/target.cpp checks/driver.cpp "${objects[@]}" -lm -o "$b/target"
 "$b/target" model joins normalization callbacks
 bash checks/run.sh baseline
fi
