#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for file in checks/spherical_contract.c examples/spherical_walk.c; do
  test -s "$file" || { echo "MISSING_TARGET_SOURCE $file" >&2; exit 1; }
done
deps="$PWD/baseline/.deps/prefix"
test -f "$deps/lib/libz.a"
cmake --build baseline/.build -j 4
cc -std=c99 -O2 -I src/hrtf -I baseline/.build/src checks/spherical_contract.c baseline/.build/src/libmysofa.a "$deps/lib/libz.a" -lm -pthread -o baseline/.build/spherical_contract
cc -std=c99 -O2 -I src/hrtf -I baseline/.build/src examples/spherical_walk.c baseline/.build/src/libmysofa.a "$deps/lib/libz.a" -lm -o baseline/.build/spherical_walk
out=$(mktemp "$PWD/.spherical-contract.XXXXXX"); trap 'rm -f "$out"' EXIT
for group in geometry audio lifetime malformed; do
  baseline/.build/spherical_contract "$group" | tee "$out"
  bash baseline/check-output.sh "$group" "$out"
done
baseline/.build/spherical_walk | tee "$out"
grep -Eq '^SPHERICAL_WALK_OK directions=[1-9][0-9]*$' "$out"
bash baseline/run.sh
printf 'SPHERICAL_CONTRACT_OK groups=4\n'
