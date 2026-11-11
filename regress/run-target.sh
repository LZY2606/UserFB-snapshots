#!/usr/bin/env bash
set -euo pipefail
names=(TestVaryCatalog TestVaryRevalidation TestVaryPublication)
pattern=$(IFS='|'; echo "${names[*]}")
list=$(bash regress/go.sh test ./regress -list "^($pattern)$")
for n in "${names[@]}"; do grep -qx "$n" <<<"$list" || { echo "missing target test: $n"; exit 1; }; done
out=$(mktemp); trap 'rm -f "$out"' EXIT
bash regress/go.sh test -count=1 -v ./regress -run "^($pattern)$" | tee "$out"
for n in "${names[@]}"; do grep -q -- "--- PASS: $n " "$out"; done
! grep -q -- '--- SKIP:' "$out"
bash regress/go.sh test -count=1 .
echo VARY_CATALOG_TARGET_OK
