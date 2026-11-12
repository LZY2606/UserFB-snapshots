#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s tests/gsub_context.c || { echo 'missing tests/gsub_context.c' >&2; exit 2; }
mkdir -p gsubcheck/.build
for mode in c99 cxx11; do
  if [ "$mode" = c99 ]; then
    ${CC:-cc} -std=c99 -DNDEBUG -O2 -I. -Igsubcheck tests/gsub_context.c gsubcheck/target_main.c -lm -o gsubcheck/.build/target
  else
    ${CXX:-c++} -x c++ -std=c++11 -DNDEBUG -O2 -I. -Igsubcheck tests/gsub_context.c gsubcheck/target_main.c -lm -o gsubcheck/.build/target
  fi
  gsubcheck/.build/target >gsubcheck/.build/target-$mode.log
  cat gsubcheck/.build/target-$mode.log
  for group in lookup_plan context_edits filter_and_render errors_and_legacy; do
    test "$(grep -c "^GROUP_OK $group checks=[1-9][0-9]*$" gsubcheck/.build/target-$mode.log)" -eq 1
  done
  test "$(grep -c '^TARGET_OK groups=4 checks=[1-9][0-9]*$' gsubcheck/.build/target-$mode.log)" -eq 1
done
sh gsubcheck/baseline.sh
