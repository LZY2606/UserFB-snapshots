#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/csmith-check.XXXXXX")
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DCMAKE_BUILD_TYPE=Release -DBSD_LIBRARY=BSD_LIBRARY-NOTFOUND > "$b/configure.log" 2>&1 || { cat "$b/configure.log"; exit 1; }
cmake --build "$b/build" --target csmith headers -j 8 > "$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
args=(--seed 42 --max-funcs 1 --max-block-size 2 --max-expr-complexity 5 --no-volatiles --no-pointers --no-arrays --no-structs --no-unions --no-jumps)
"$b/build/src/csmith" "${args[@]}" > "$b/sample.c"
for opt in 0 2; do
  cc -std=c11 -w -O"$opt" -Iruntime -I"$b/build/runtime" "$b/sample.c" -o "$b/sample$opt"
  "$b/sample$opt" > "$b/result$opt"
done
cmp "$b/result0" "$b/result2"
grep -q '^checksum = [0-9A-Fa-f][0-9A-Fa-f]*$' "$b/result0"
cat "$b/result0"
echo CSMITH_COMPILE_EXECUTE_OK seed=42 optimizations=2
if [[ ${1:-baseline} == target ]]; then
  c++ -std=c++20 -DHAVE_CONFIG_H -Dmain=csmith_original_main -I"$b/build" -Isrc -I"$b/build/src" -c src/RandomProgramGenerator.cpp -o "$b/renamed-main.o"
  objs=()
  while IFS= read -r f; do objs+=("$f"); done < <(find "$b/build/src/CMakeFiles/csmith.dir" -name '*.o' ! -name 'RandomProgramGenerator.cpp.o' | sort)
  c++ -std=c++20 -DHAVE_CONFIG_H -I"$b/build" -Isrc -I"$b/build/src" checks/generic_driver.cpp tests/generic_expression.cpp "${objs[@]}" "$b/renamed-main.o" -o "$b/target"
  export CSMITH_CHECK_BIN="$b/build/src/csmith" CSMITH_CHECK_RUNTIME="$PWD/runtime" CSMITH_CHECK_GENERATED_RUNTIME="$b/build/runtime" CSMITH_CHECK_TMP="$b"
  "$b/target" | tee "$b/target.log"
  for group in types effects facts clone generated compatibility; do
    grep -Eq "^PASS $group checks=[1-9][0-9]*$" "$b/target.log"
  done
  grep -Eq "^CSMITH_GENERIC_OK groups=6 checks=[1-9][0-9]*$" "$b/target.log"
fi
