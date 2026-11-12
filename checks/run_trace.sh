#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
cmake -S . -B "$build" -DPTUNIT=ON -DBUILD_SHARED_LIBS=OFF -DFEATURE_THREADS=OFF -DPTXED=OFF -DPTTC=OFF -DPTDUMP=OFF -DMAN=OFF >"$build/config.log" 2>&1
cmake --build "$build" -j2 >"$build/build.log" 2>&1
cc -std=c99 -I"$build/libipt/include" checks/packet_baseline.c "$build/lib/libipt.a" -o "$build/witness"
"$build/witness"
ctest --test-dir "$build" -R 'ptunit-(query|retstack|tnt_cache|insn_decoder|block_decoder)$' --output-on-failure
if [[ ${1:-baseline} == target ]]; then
 test -s checks/return_trace.c && test -s checks/return_mutations.sh
 cc -std=c99 -I"$build/libipt/include" checks/return_trace.c "$build/lib/libipt.a" -o "$build/target"
 "$build/target" | tee "$build/target.log"
 total=0
 for n in nested direct_indirect call_next tnt_boundary warm_cache explicit_tip; do
  count=$(awk -v group="$n" '$1=="PASS" && $2==group && $3 ~ /^cases=[1-9][0-9]*$/ && NF==3 {sub(/^cases=/,"",$3); value=$3; found++} END {if(found!=1) exit 1; print value}' "$build/target.log")
  total=$((total + count))
 done
 (( total >= 48 ))
 bash checks/return_mutations.sh | tee "$build/mutations.log"
 for n in call_fallthrough cached_call tnt_return; do
  grep -Fx "KILLED $n" "$build/mutations.log" >/dev/null
 done
 printf 'RETURN_TRACE_OK groups=6 cases=%s mutations=3\n' "$total"
fi
