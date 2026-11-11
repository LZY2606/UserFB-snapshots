#!/usr/bin/env bash
set -euo pipefail
root=$PWD
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cp -R dwarf elf "$build/"
make -C "$build/elf" CXXFLAGS='-O1' libelf++.a
make -C "$build/dwarf" CXXFLAGS='-O1' libdwarf++.a
clang++ -std=c++11 -I. checks/baseline.cpp "$build/dwarf/libdwarf++.a" "$build/elf/libelf++.a" -o "$build/baseline"
"$build/baseline"
clang++ -std=c++11 -Ielf -Idwarf examples/dump-sections.cc "$build/elf/libelf++.a" -o "$build/sections"
"$build/sections" test/golden-gcc-4.9.2/example > "$build/sections.out"
cmp "$build/sections.out" test/golden-gcc-4.9.2/sections
echo ELF_GOLDEN_SECTIONS_OK
if [[ ${1:-} == target ]]; then
  test -s checks/composite_locations.cpp
  clang++ -std=c++11 -I. checks/composite_locations.cpp "$build/dwarf/libdwarf++.a" "$build/elf/libelf++.a" -o "$build/composite"
  "$build/composite" | tee "$build/target.log"
  grep -qx 'COMPOSITE_LOCATIONS_OK cases=8' "$build/target.log"
fi
