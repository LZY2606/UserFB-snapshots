#!/usr/bin/env bash
set -euo pipefail
test -s tests/layer_contract.c
test -s tests/run_layer_contract.sh
o=$(mktemp)
trap 'unlink "$o"' EXIT
bash tests/run_layer_contract.sh | tee "$o"
for c in wb-nb-fixed wb-nb-changing uwb-nb-fixed uwb-nb-changing uwb-wb-fixed uwb-wb-changing controls-ignored controls-callback; do
 grep -Fxq "PASS $c" "$o"
done
for m in skip control; do grep -Fq "DETECTED_$m" "$o";done
bash devcheck/baseline.sh
