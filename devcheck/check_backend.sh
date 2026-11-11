#!/bin/bash
# Per-instance backend dispatch regression:
#   * builds and runs devcheck/backend_contract.c under default,
#     MINIMP3_RUNTIME_DISPATCH, MINIMP3_NO_SIMD and explicit
#     MINIMP3_ONLY_SIMD configurations (plus a float-output variant);
#   * every configuration runs to completion, constrained builds exercise
#     their scalar path and the setter-failure branches instead of skipping;
#   * on machines with SSE2/NEON the runtime-dispatch build records real
#     scalar and SIMD kernel frames through the test hook;
#   * the legacy native/NO_SIMD PCM baseline runs as well.
# Prints each executed configuration, per-vector frame counts and finally
# MINIMP3_BACKEND_OK on full success.
set -euo pipefail
cd "$(dirname "$0")/.."

build=$(mktemp -d "${TMPDIR:-/tmp}/minimp3-backend.XXXXXX")
trap 'rm -rf "$build"' EXIT
cflags="-std=c99 -Wall -Wextra -Werror -O2 -I."

run_config()
{
    name=$1
    shift
    echo "== config: $name (cc $*) =="
    cc $cflags "$@" devcheck/backend_contract.c -lm -o "$build/contract"
    "$build/contract" | tee "$build/out.$name.txt" | grep -E '^(config:|vectors/)'
    grep -q '^MINIMP3_BACKEND_OK$' "$build/out.$name.txt"
}

run_config default
run_config runtime-dispatch -DMINIMP3_RUNTIME_DISPATCH
run_config no-simd -DMINIMP3_NO_SIMD
run_config only-simd -DMINIMP3_ONLY_SIMD
# explicit ONLY_SIMD must remain a hard restriction even with dispatch enabled
run_config only-simd-with-dispatch -DMINIMP3_ONLY_SIMD -DMINIMP3_RUNTIME_DISPATCH
run_config runtime-dispatch-float -DMINIMP3_RUNTIME_DISPATCH -DMINIMP3_FLOAT_OUTPUT

# public headers must also be valid C++ (compile-only, no extra link deps)
echo "== config: c++ syntax (runtime-dispatch, no-simd) =="
cc -x c++ -std=c++11 -Wall -Wextra -Werror -O2 -I. -DMINIMP3_RUNTIME_DISPATCH -fsyntax-only devcheck/backend_contract.c
cc -x c++ -std=c++11 -Wall -Wextra -Werror -O2 -I. -DMINIMP3_NO_SIMD -fsyntax-only devcheck/backend_contract.c
echo "c++ syntax OK"

# legacy baseline: native and MINIMP3_NO_SIMD decode against reference PCM
echo "== legacy baseline =="
bash devcheck/run.sh

echo MINIMP3_BACKEND_OK
