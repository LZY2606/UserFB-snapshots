#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/onig-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DBUILD_SHARED_LIBS=OFF -DBUILD_TEST=ON -DINSTALL_DOCUMENTATION=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_FLAGS=-DONIG_ATOM_PREFILTER_TEST=1 > "$b/configure.log" 2>&1 || { cat "$b/configure.log"; exit 1; }
cmake --build "$b/build" --target onig test_syntax -j 8 > "$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c11 -Isrc -I"$b/build" checks/witness.c "$b/build/libonig.a" -o "$b/base"
"$b/base"
"$b/build/test/test_syntax" > "$b/syntax.log" 2>&1 || { cat "$b/syntax.log"; exit 1; }
tail -3 "$b/syntax.log"
echo ONIG_EXISTING_SYNTAX_OK
if [[ ${1:-baseline} == target ]]; then
  cc -std=c11 -DONIG_ATOM_PREFILTER_TEST=1 -Isrc -I"$b/build" checks/atom_driver.c test/atom_prefilter.c "$b/build/libonig.a" -o "$b/target"
  "$b/target" | tee "$b/target.log"
  for group in coverage intervals priority fallback reuse work; do
    grep -Eq "^PASS $group checks=[1-9][0-9]*$" "$b/target.log"
  done
  grep -Eq "^ONIG_ATOM_PREFILTER_OK groups=6 checks=[1-9][0-9]*$" "$b/target.log"
fi
