#!/usr/bin/env bash
set -euo pipefail
mkdir -p build
f=tests/compound_attachment.c
[[ -s "$f" ]] || { echo "Missing nonempty $f" >&2; exit 2; }
for variant in c99 cpp11 release; do
  case "$variant" in
    c99) cc -std=c99 -O1 -g -I. "$f" -lm -o build/compound-target ;;
    cpp11) c++ -x c++ -std=c++11 -O1 -g -I. "$f" -lm -o build/compound-target ;;
    release) cc -std=c99 -DNDEBUG -O2 -I. "$f" -lm -o build/compound-target ;;
  esac
  build/compound-target | tee "build/target-$variant.log"
  for name in raw_points nested_affine unsigned_indices offset_modes render_equivalence failure_cleanup; do
    [[ $(grep -Fxc "PASS $name" "build/target-$variant.log") == 1 ]] || { echo "Missing/duplicate executed case: $name" >&2; exit 3; }
  done
  grep -Fx 'COMPOUND_ATTACHMENT_OK cases=6' "build/target-$variant.log" >/dev/null
done
