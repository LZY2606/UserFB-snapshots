#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case ${1:-baseline} in baseline|target) ;; *) echo 'usage: run.sh baseline|target' >&2; exit 2;; esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
sources=(src/common/dwarf/dwarf2reader.cc src/common/dwarf/bytereader.cc src/common/dwarf/elf_reader.cc)
"${CXX:-c++}" -std=c++17 -O1 -Isrc support/range_baseline.cc "${sources[@]}" -o "$work/range_baseline"
"$work/range_baseline"
"${CXX:-c++}" -std=c++17 -O1 -Isrc support/baseline.cc "${sources[@]}" -o "$work/baseline"
"$work/baseline" samples/optimized4.elf samples/optimized5.elf
if [[ ${1:-baseline} == target ]]; then
 for file in src/common/dwarf/location_list_reader.h src/common/dwarf/location_list_reader.cc src/common/dwarf/variable_location.h src/common/dwarf/variable_location.cc tools/variable_locations.cc tests/location_contract.cc analysis/variables.md; do
  if [[ ! -s $file ]]; then echo "missing required file: $file" >&2; exit 2; fi
 done
 added=(src/common/dwarf/location_list_reader.cc src/common/dwarf/variable_location.cc)
 "${CXX:-c++}" -std=c++17 -O1 -Isrc -Isupport tests/location_contract.cc support/contract_driver.cc "${added[@]}" "${sources[@]}" -o "$work/contract"
 bash support/contract_gate.sh "$work/contract" "$work/contract.receipt"
 "${CXX:-c++}" -std=c++17 -O1 -Isrc tools/variable_locations.cc "${added[@]}" "${sources[@]}" -o "$work/variable_locations"
 "$work/variable_locations" samples/queries.tsv > "$work/variables.tsv"
 "${CXX:-c++}" -std=c++17 -O1 -Isrc support/report_check.cc "${sources[@]}" -o "$work/report_check"
 "$work/report_check" samples/queries.tsv "$work/variables.tsv"
 mkdir -p analysis
 cp "$work/variables.tsv" analysis/variables.tsv
 echo 'VARIABLE_ANALYSIS_OK'
fi
