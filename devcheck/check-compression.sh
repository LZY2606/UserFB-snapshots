#!/usr/bin/env bash
set -euo pipefail
test -s test/model_space_compression.cpp
out=$(mktemp); trap 'rm -f "$out"' EXIT
bash devcheck/run-current.sh test/model_space_compression.cpp | tee "$out"
for marker in MODELSPACE_LONG_CHAIN_OK MODELSPACE_GLOBAL_ERROR_OK MODELSPACE_REDUCTION_OK MODELSPACE_SKINNING_OK MODELSPACE_INVALID_OK; do grep -qx "$marker" "$out"; done
bash devcheck/run-current.sh
printf 'CAL3D_MODELSPACE_OK\n'
