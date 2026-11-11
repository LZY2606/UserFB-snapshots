#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d 2>/dev/null) || build=$(mktemp -d "${TMPDIR:-/tmp}/relr_contract.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O0 -I. tests/relr_contract.cpp -o "$build/relr_contract"
out=$("$build/relr_contract")
echo "$out"
for group in decode_4combo empty_bitmap_run msb_bitmap unaligned_location \
             nobits_preview overlap_and_truncated encoding_overflow \
             preview_progbits dump_correspondence save_roundtrip; do
    grep -q "^PASS ${group}\$" <<<"$out"
done
echo RELR_CONTRACT_OK
