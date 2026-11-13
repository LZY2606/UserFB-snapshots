#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
cc=${CC:-cc}
mode=${1:-baseline}
flags=(-std=c99 -O2 -I. -Itest -Isupport)
case "$mode" in
 baseline)
  "$cc" "${flags[@]}" support/baseline.c -lm -o build/baseline
  build/baseline
  "$cc" "${flags[@]}" test/test_shapes.c test/console-colors.c -lm -o build/upstream-shapes
  build/upstream-shapes
  echo 'BASELINE upstream-test_shapes OK'
  ;;
 target)
  test -s support/target.c
  test -s examples/adaptive.c
  for bits in 16 32; do
   extra=(); if [[ "$bits" == 32 ]]; then extra=(-include stdint.h -DPAR_SHAPES_T=uint32_t); fi
   "$cc" "${flags[@]}" ${extra[@]+"${extra[@]}"} support/driver.c support/target.c -lm -o build/target
   for group in refine transition seams failure; do build/target "$group"; done
  done
  "$cc" "${flags[@]}" examples/adaptive.c -lm -o build/adaptive
  build/adaptive build
  test -s build/adaptive.obj
  bash support/run.sh baseline
  ;;
 *) echo 'usage: bash support/run.sh baseline|target' >&2; exit 2;;
esac
