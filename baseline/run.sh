#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "$PWD/baseline/.build.XXXXXX")
trap 'find "$build" -type f -delete; rmdir "$build"' EXIT
sources=(mnemonics textdefs prefix operands insts instructions distorm decoder)
objects=()
for name in "${sources[@]}"; do
  cc -std=c99 -O2 -DSUPPORT_64BIT_OFFSET -DDISTORM_STATIC -Iinclude -c "src/$name.c" -o "$build/$name.o"
  objects+=("$build/$name.o")
done
cc -std=c99 -O2 -Iinclude baseline/witness.c "${objects[@]}" -o "$build/witness"
"$build/witness"
c++ -Wno-ignored-pragmas -Wno-format -Iinclude examples/tests/main.cpp "${objects[@]}" -o "$build/old-sample"
"$build/old-sample" > "$build/old.out"
cat "$build/old.out"
grep -q 'STAC' "$build/old.out"
grep -qi '0f01cb' "$build/old.out"
printf '%s\n' 'OLD_SAMPLE_OK'
