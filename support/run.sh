#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
 baseline) test_src=support/baseline.cc; extra_src= ;;
 target) test_src=test/frame_move_cases.cc; extra_src=support/main.cc
 test -s "$test_src" || { echo "missing nonempty $test_src" >&2; exit 64; } ;;
 *) exit 64 ;;
esac
mkdir -p .offline-build
c++ -std=c++11 -O1 -I. -Idwarf -Ielf dwarf/*.cc elf/*.cc \
 support/elf-enums.cc support/dwarf-enums.cc "$test_src" $extra_src -o .offline-build/run
.offline-build/run
