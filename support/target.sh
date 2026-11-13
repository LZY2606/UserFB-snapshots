#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh support/driver.c tests/batch_contract.c -o "$b/contract"
"$b/contract" | tee "$b/out"
awk -v style=B -v groups="valid equations encodings workspace work" -f support/verify_output.awk "$b/out"
bash support/baseline.sh
