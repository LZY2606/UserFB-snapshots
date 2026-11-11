#!/usr/bin/env bash
set -euo pipefail
test -s tests/layout_contract.cpp
test -s tests/run-layout-contract.sh
out=$(mktemp); trap 'rm -f "$out"' EXIT
bash tests/run-layout-contract.sh --mutations | tee "$out"
grep -Eq '^LAYOUT_NORMAL_OK cases=[1-9][0-9]* assertions=[1-9][0-9]*$' "$out"
for id in matrix-major nested-offset bytecode-remap; do grep -Eq "^MUTATION_CAUGHT id=$id compiled=1 executed=1 assertion=.+$" "$out"; done
bash checks/run-current.sh
printf 'REFLECT_LAYOUT_TARGET_OK\n'
