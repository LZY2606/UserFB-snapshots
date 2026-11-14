#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ "$mode" == baseline || "$mode" == target ]]
build=$(mktemp -d "${TMPDIR:-/tmp}/bdw-lifetime.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc=${CC:-cc}
for variant in serial tls; do
  opts=(); defs=(-DZ1542_SERIAL_BUILD)
  if [[ "$variant" == tls ]]; then
    opts=(-Denable_threads=ON -Denable_thread_local_alloc=ON)
    defs=(-DGC_THREADS -pthread)
  else
    opts=(-Denable_threads=OFF -Denable_thread_local_alloc=OFF)
  fi
  if [[ "$mode" == target ]]; then opts+=(-Denable_lifetime_tracking=ON); fi
  cmake -S . -B "$build/$variant" -DCMAKE_C_COMPILER="$cc" -DGC_BUILD_SHARED_LIBS=OFF -Denable_parallel_mark=OFF -Denable_cplusplus=OFF -Dbuild_cord=OFF -Denable_docs=OFF -DBUILD_TESTING=ON -Dwith_libatomic_ops=OFF -Dwithout_libatomic_ops=OFF "${opts[@]}"
  cmake --build "$build/$variant" --target gc realloctest middletest gctest -j 2
  "$cc" -std=c99 -O2 "${defs[@]}" -I include baseline/current.c "$build/$variant/libgc.a" -o "$build/$variant/current"
  "$build/$variant/current"
  if [[ "$mode" == target ]]; then
    "$cc" -std=c99 -O2 "${defs[@]}" -I include -I baseline baseline/driver.c tests/lifetime_contract.c "$build/$variant/libgc.a" -o "$build/$variant/target"
    "$build/$variant/target" | tee "$build/$variant/target.log"
    for group in identity realloc collection; do
      grep -Eq "^GROUP_PASS $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$build/$variant/target.log"
    done
    ng=3
    if [[ "$variant" == tls ]]; then
      ng=4
      grep -Eq '^GROUP_PASS threads cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/$variant/target.log"
    fi
    grep -Eq "^CONTRACT_DONE cases=[1-9][0-9]* groups=$ng checks=[1-9][0-9]*$" "$build/$variant/target.log"
  fi
  ctest --test-dir "$build/$variant" --output-on-failure --no-tests=error -R '^(realloctest|middletest|gctest)$'
  printf 'OLD_TESTS_OK variant=%s count=3\n' "$variant"
done
