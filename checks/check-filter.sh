#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for f in tests/filter_contract.cpp examples/filter_demo.cpp; do
    if ! test -s "$f"; then printf 'MISSING_TARGET %s\n' "$f" >&2; exit 2; fi
done
bash checks/install.sh
bash checks/legacy.sh
mkdir -p build-filter/results
build-filter/filter_contract > build-filter/results/contract.log
cat build-filter/results/contract.log
awk -f checks/verify-filter-log.awk build-filter/results/contract.log
build-filter/filter_demo build-filter/results/filter-demo.png > build-filter/results/demo.log
cat build-filter/results/demo.log
grep -qx 'FILTER_DEMO_OK' build-filter/results/demo.log
test -s build-filter/results/filter-demo.png
test "$(od -An -tx1 -N8 build-filter/results/filter-demo.png | tr -d ' \n')" = 89504e470d0a1a0a
printf '%s\n' FILTER_TARGET_OK
