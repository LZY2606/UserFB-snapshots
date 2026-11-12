#!/usr/bin/env bash
# Build three semantic mutants of xmlschemas.c in temporary source copies
# and verify each one: it must compile, normal data must still validate,
# and the targeted content case must show a clear deviation.  A compile
# failure is a harness bug, not a finding; the pristine source tree is
# never patched.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/idc-mutants.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

for m in dupkey keymissing keyref; do
  src="$tmp/src-$m"
  mkdir -p "$src"
  # copy the tracked source tree (working-tree content) into a scratch copy
  (cd "$repo" && git ls-files -z | tar --null -T - -cf -) | tar -xf - -C "$src"
  patch -s -p1 -d "$src" < "$here/$m.patch"
  (cd "$src" && bash regress/build.sh "$tmp/build-$m" > /dev/null)
  cc -std=c99 -I "$src/include" -I "$tmp/build-$m" \
     "$here/driver.c" "$tmp/build-$m/libxml2.a" -lm -pthread \
     -o "$tmp/driver-$m"
  out=$("$tmp/driver-$m" "$m")
  echo "$out"
  echo "$out" | grep -q "^DEVIATION $m$"
  echo "MUTANT $m DEVIATION_CONFIRMED"
done
