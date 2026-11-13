#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s checks/it_decode_contract.cpp || { echo 'missing checks/it_decode_contract.cpp' >&2; exit 1; }
build=$(mktemp -d checks/.build.XXXXXX)
trap 'rm -rf "$build"' EXIT
bash checks/build.sh "$build"
"${CXX:-c++}" -std=c++17 -O1 -g -Iinclude -Ichecks ${CHECK_CFLAGS:-} checks/contract_main.cpp checks/it_decode_contract.cpp "$build/libdumb.a" -lm -o "$build/contract"
echo IT_TARGET_BUILD_OK
"$build/contract" | tee "$build/target.stdout"
awk -f checks/verify-output.awk "$build/target.stdout"
"${CXX:-c++}" -std=c++17 -O1 -g -Iinclude ${CHECK_CFLAGS:-} checks/baseline.cpp "$build/libdumb.a" -lm -o "$build/legacy"
"$build/legacy" --legacy
echo IT_TARGET_OK
