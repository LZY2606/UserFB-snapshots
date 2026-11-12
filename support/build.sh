#!/bin/bash
set -euo pipefail
build=$1
mkdir -p "$build"
src=(wrapper.c common/cmnMemory.c amrwbenc/src/*.c)
if [[ ${2:-baseline} == target ]]; then src+=(support/target_main.c tests/pulse_contract.c); flags="-DPULSE_TRACE -Itests"; else src+=(support/baseline.c); flags=""; fi
cc -O2 -std=c99 $flags -I. -Icommon/include -Iamrwbenc/inc -Isupport "${src[@]}" -lm -o "$build/run"
