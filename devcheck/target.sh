#!/bin/bash
set -euo pipefail
bash devcheck/target_core.sh
b=$(mktemp -d "${TMPDIR:-/tmp}/tta-mutations.XXXXXX")
trap 'rm -rf "$b"' EXIT
source devcheck/build.sh
for variant in encoder_k decoder_base; do
 TTA_INTERNAL=1 TTA_VARIANT=$variant build_case tests/entropy_reference.c devcheck/target_main.c
 if "$b/case" > "$b/output" 2>&1; then echo "MISSED $variant"; exit 41; fi
 cat "$b/output"
 grep -q '^ENTROPY_MISMATCH ' "$b/output"
 echo "TTA_MUTANT $variant CONTENT_REJECTED"
done
echo TTA_ENTROPY_REGRESSION_OK
