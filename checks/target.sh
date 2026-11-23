#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source checks/build.sh checks/contract_main.cpp checks/sidechain_contract.cpp
"$build/run" | tee "$build/output"
for group in envelope graph clock lifetime; do
    grep -Eq "^PASS $group/[^ ]+ [1-9][0-9]*$" "$build/output"
    grep -Eq "^GROUP_DONE $group [1-9][0-9]* [1-9][0-9]*$" "$build/output"
done
grep -Eq '^CONTRACT_COMPLETE [1-9][0-9]* [1-9][0-9]*$' "$build/output"
bash checks/baseline.sh
bash checks/upstream.sh
