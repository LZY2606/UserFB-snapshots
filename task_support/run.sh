#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/cgltf-skin.XXXXXX")
trap 'rm -rf "$work"' EXIT
cc_bin=${CC:-cc}
cxx_bin=${CXX:-c++}
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
"$cc_bin" -std=c99 -O2 "$root/task_support/fixture.c" -o "$work/fixture"
cd "$work"
./fixture
"$cc_bin" -std=c99 -O2 "$root/task_support/baseline.c" -lm -o baseline
./baseline
for name in main test_math test_strings test_conversion test_write test_write_glb; do
  if [[ "$name" == main ]]; then
    "$cc_bin" -std=c99 -O2 "$root/test/main.c" -o "$work/$name"
  else
    "$cxx_bin" -std=c++11 -O2 "$root/test/$name.cpp" -o "$work/$name"
  fi
  "$work/$name" scene.gltf
  printf 'UPSTREAM %s PASS\n' "$name"
done
printf 'UPSTREAM_SELECTED_6_OK\n'
if [[ "$mode" == baseline ]]; then exit 0; fi
for file in examples/cgltf_skin_bvh.h examples/cgltf_skin_bvh.c examples/skin_bvh_example.c test/skin_bvh_test.c; do
  if [[ ! -s "$root/$file" ]]; then printf 'missing or empty %s\n' "$file" >&2; exit 4; fi
done
"$cc_bin" -std=c99 -O2 -Wall -Wextra -I"$root" "$root/task_support/cgltf_impl.c" \
  "$root/examples/cgltf_skin_bvh.c" "$root/test/skin_bvh_test.c" "$root/task_support/driver.c" -lm -o target
set +e
./target > target.stdout 2> target.stderr
status=$?
set -e
cat target.stdout
cat target.stderr >&2
if [[ $status -ne 0 ]]; then exit "$status"; fi
# Verify actual per-case checks, all group summaries and final completion outside the target process.
awk '
$1=="CASE" && NF==5 && $4=="CHECKS" && $5~/^[1-9][0-9]*$/ {
 if ($2!="deform" && $2!="bounds" && $2!="query") exit 1;
 if (seen[$3]++) exit 1; count[$2]++; checks[$2]+=$5; total++; next;
}
$1=="GROUP" && NF==8 && $3=="CASES" && $5=="CHECKS" && $7=="COMPLETE" {exit 1}
$1=="GROUP" && NF==7 && $3=="CASES" && $5=="CHECKS" && $7=="COMPLETE" {
 if (done[$2]++ || $4!=count[$2] || $6!=checks[$2] || $4<1 || $6<1) exit 1; next;
}
$0=="TARGET_ALL_COMPLETE" {final++; next;}
END {if(total<6 || done["deform"]!=1 || done["bounds"]!=1 || done["query"]!=1 || final!=1) exit 1;}
' target.stdout
"$cc_bin" -std=c99 -O2 -Wall -Wextra -I"$root" "$root/task_support/cgltf_impl.c" \
  "$root/examples/cgltf_skin_bvh.c" "$root/examples/skin_bvh_example.c" -lm -o example
./example scene.gltf > example.stdout
cat example.stdout
grep -Fx 'SKIN_EXAMPLE_HIT' example.stdout
printf 'SKIN_BVH_TARGET_OK\n' 
