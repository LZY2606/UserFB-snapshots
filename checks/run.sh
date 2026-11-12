#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/fdt-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
sources=(libfdt/fdt.c libfdt/fdt_ro.c libfdt/fdt_wip.c libfdt/fdt_sw.c libfdt/fdt_rw.c libfdt/fdt_strerror.c libfdt/fdt_empty_tree.c libfdt/fdt_addresses.c libfdt/fdt_overlay.c libfdt/fdt_check.c)
cc -std=c11 -D_DEFAULT_SOURCE -Ilibfdt "${sources[@]}" checks/witness.c -o "$b/base"
"$b/base"
if [[ ${1:-baseline} == target ]]; then
  if [[ -f libfdt/fdt_overlay_delta.c ]]; then sources+=(libfdt/fdt_overlay_delta.c); fi
  cc -std=c11 -D_DEFAULT_SOURCE -Ilibfdt "${sources[@]}" checks/delta_driver.c tests/overlay_delta.c -o "$b/target"
  "$b/target" | tee "$b/target.log"
  for group in diff external local renumber roundtrip reject; do
    grep -Eq "^PASS $group checks=[1-9][0-9]*$" "$b/target.log"
  done
  grep -Eq "^OVERLAY_DELTA_OK groups=6 checks=[1-9][0-9]*$" "$b/target.log"
fi
