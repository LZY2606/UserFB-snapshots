#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
clang -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. mjs.c regress/baseline.c -lm -o "$build/base"
"$build/base"
clang -std=c99 -O0 -DMJS_ENABLE_DEBUG=1 -DMJS_MEMORY_STATS -I. -Isrc tests/unit_test.c src/common/cs_dirent.c src/common/cs_time.c src/common/test_main.c src/common/test_util.c -lm -o "$build/old"
TEST_SEED=1 "$build/old" test_foreign_str | tee "$build/old.log"
grep -q 'PASS, ran 1 tests' "$build/old.log"
echo MJS_OLD_FOREIGN_STRING_OK
if [[ ${1:-} == target ]]; then
  test -s regress/gc_sequence.c
  test -s regress/gc_model.c
  test -s regress/gc_sequence_test.c
  bash regress/run_gc_sequence.sh | tee "$build/sequence.log"
  grep -qx 'MJS_GC_SEQUENCE_OK cases=8 mutants=2' "$build/sequence.log"
fi
