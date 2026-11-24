#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ "$mode" == baseline || "$mode" == target ]] || { echo 'use baseline or target' >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rustc --edition=2024 --crate-name gimli --crate-type=rlib --cfg 'feature="read"' --cfg 'feature="read-core"' --cfg 'feature="std"' src/lib.rs -o "$work/libgimli.rlib"
rustc --edition=2024 --extern "gimli=$work/libgimli.rlib" support/baseline.rs -o "$work/baseline"
"$work/baseline"
if [[ "$mode" == target ]]; then
 test -s tests/segmented_contract.rs || { echo 'missing tests/segmented_contract.rs' >&2; exit 1; }
 test -s tools/segmented_lookup.rs || { echo 'missing tools/segmented_lookup.rs' >&2; exit 1; }
 rustc --edition=2024 --extern "gimli=$work/libgimli.rlib" support/contract_main.rs -o "$work/contract"
 export UFB_COMPLETION_PATH="$work/completed"
 export UFB_COMPLETION_TOKEN="$(basename "$work")-$$"
 "$work/contract" > "$work/contract.stdout"
 cat "$work/contract.stdout"
 test -s "$UFB_COMPLETION_PATH"
 grep -Eq "^$UFB_COMPLETION_TOKEN cases=5 comparisons=[1-9][0-9]*$" "$UFB_COMPLETION_PATH"
 for name in aranges_alignment line_selector sequence_identity flat_compatibility bank_attribution; do
  grep -Eq "^CASE_OK $name comparisons=[1-9][0-9]*$" "$work/contract.stdout"
 done
 rustc --edition=2024 --extern "gimli=$work/libgimli.rlib" tools/segmented_lookup.rs -o "$work/lookup"
 "$work/lookup" fixtures/banked-fw fixtures/banked-fw/queries.tsv > "$work/report.tsv"
 rustc --edition=2024 support/check_report.rs -o "$work/check-report"
 "$work/check-report" fixtures/banked-fw fixtures/banked-fw/queries.tsv "$work/report.tsv"
 cat "$work/report.tsv"
 echo 'SEGMENTED_ANALYSIS_DRIVER_OK'
fi
