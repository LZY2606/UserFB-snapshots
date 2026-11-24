#!/bin/bash
# Optional fixture regeneration only; normal workflow uses the supplied ELFs.
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for version in 4 5; do
 for unit in alpha beta; do
  "${CLANGXX:-clang++}" --target=x86_64-linux-gnu -O2 -gdwarf-"$version" -fno-omit-frame-pointer -ffile-prefix-map="$PWD"=. -fdebug-compilation-dir=. -c "samples/src/$unit.cc" -o "$work/$unit.o"
 done
 "${CLANGXX:-clang++}" --target=x86_64-linux-gnu -O2 -c samples/src/observe.cc -o "$work/observe.o"
 if [[ $version == 4 ]]; then base=0x401000; else base=0x501000; fi
 "${ELF_LD:-ld.lld}" -m elf_x86_64 --build-id=none -e recover_alpha -Ttext="$base" "$work/alpha.o" "$work/beta.o" "$work/observe.o" -o "samples/optimized$version.elf"
done
