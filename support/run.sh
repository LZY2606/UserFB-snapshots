#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "$(pwd)/.uf-build.XXXXXX")
export TMPDIR="$b"
trap 'rm -rf "$b"' EXIT
cc=${CC:-cc}
cxx=${CXX:-c++}
baseline() {
  "$cc" -std=c89 -O2 -DLODEPNG_NO_COMPILE_CPP -x c -I. lodepng.cpp support/baseline.c -o "$b/baseline"
  "$b/baseline"
}
old() {
  "$cxx" -std=c++11 -O2 -I. lodepng.cpp lodepng_util.cpp support/old_compare.cpp -o "$b/old"
  "$b/old" > "$b/old.log"
  cat "$b/old.log"
  grep -Fxq 'LODEPNG_OLD_SELECTED_PASS groups=6' "$b/old.log"
}
case "${1:-baseline}" in
  baseline) baseline ;;
  old) old ;;
  target)
    test -s tests/row_stream.cpp
    "$cc" -std=c89 -O2 -DLODEPNG_NO_COMPILE_CPP -DLODEPNG_NO_COMPILE_ALLOCATORS -x c -I. -c lodepng.cpp -o "$b/codec.o"
    "$cc" -std=c89 -O2 -c support/alloc.c -o "$b/alloc.o"
    "$cxx" -std=c++11 -O2 -I. -Isupport support/main.cpp tests/row_stream.cpp "$b/codec.o" "$b/alloc.o" -o "$b/target"
    "$b/target" > "$b/target.log"
    cat "$b/target.log"
    for group in deflate rows framing; do
      grep -Eq "^GROUP $group PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$b/target.log"
    done
    grep -Eq '^LODEPNG_TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$b/target.log"
    "$cc" -std=c89 -O2 -DLODEPNG_NO_COMPILE_CPP -I. examples/example_row_stream.c "$b/codec.o" "$b/alloc.o" -o "$b/example"
    baseline
    old
    ;;
  *) echo 'usage: bash support/run.sh baseline|old|target' >&2; exit 2 ;;
esac
