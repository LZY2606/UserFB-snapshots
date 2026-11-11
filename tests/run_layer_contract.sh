#!/usr/bin/env bash
# Layer-contract regression orchestration.
#
# Phase 1 builds tests/layer_contract.c against the pristine tree and requires
# every group to pass.  Phase 2 copies the tree into independent temporary
# workspaces, applies each devcheck/mutations patch there, rebuilds with
# devcheck/build.sh and re-runs the same binary.  A mutation counts as caught
# only when the modified library compiles and the test then fails at runtime
# with a bit-position / frame-count / PCM assertion.  The working tree is
# never modified: patches are applied to the copies only.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
test_file=tests/layer_contract.c
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# --- Phase 1: pristine tree must pass -------------------------------------
clean_bin="$work/layer_contract"
bash "$root/devcheck/build.sh" "$root" "$root/$test_file" "$clean_bin"
set +e
"$clean_bin" | tee "$work/clean.log"
clean_rc=${PIPESTATUS[0]}
set -e
if [ "$clean_rc" -ne 0 ]; then
   echo "ERROR: pristine tree failed the layer contract" >&2
   exit 1
fi
for c in wb-nb-fixed wb-nb-changing uwb-nb-fixed uwb-nb-changing \
         uwb-wb-fixed uwb-wb-changing controls-ignored controls-callback; do
   grep -Fxq "PASS $c" "$work/clean.log" || {
      echo "ERROR: missing PASS $c on the pristine tree" >&2
      exit 1
   }
done

# --- Phase 2: each mutation must be caught at runtime ----------------------
# Runtime failure kinds that prove a real bitstream/PCM regression (the C
# test emits these with frame, layer, bit position and first bad sample).
failure_re='^FAIL group=.* kind=(TRUNC|PCM|RANGE|TERM|CALLBACK) '

run_mutation() {
   local name=$1 patch=$2 marker=$3
   local copy="$work/copy-$name"
   local bin="$work/mut-$name"
   local log="$work/mut-$name.log"

   # Independent copy: the original tree is never patched in place.
   cp -R "$root" "$copy"

   if ! patch -p1 -d "$copy" < "$patch" >/dev/null; then
      echo "ERROR: mutation $name did not apply" >&2
      exit 1
   fi

   # Compile failure is an infrastructure error, never a detection.
   if ! bash "$copy/devcheck/build.sh" "$copy" "$copy/$test_file" "$bin" \
         >"$work/build-$name.log" 2>&1; then
      echo "ERROR: mutated tree $name failed to compile" >&2
      cat "$work/build-$name.log" >&2
      exit 1
   fi

   set +e
   "$bin" >"$log" 2>&1
   local rc=$?
   set -e
   if [ "$rc" -eq 0 ]; then
      echo "ERROR: mutation $name was not detected (test passed)" >&2
      cat "$log" >&2
      exit 1
   fi
   if ! grep -Eq "$failure_re" "$log"; then
      echo "ERROR: mutation $name failed without a runtime assertion" >&2
      cat "$log" >&2
      exit 1
   fi
   # Show the concrete evidence (bit position / frame / first bad sample).
   grep -E "$failure_re" "$log" | sed "s/^/[$name] /"
   echo "$marker"
}

run_mutation skip    "$root/devcheck/mutations/skip.patch"    DETECTED_skip
run_mutation control "$root/devcheck/mutations/control.patch" DETECTED_control

echo "LAYER_CONTRACT_SUITE_OK"
