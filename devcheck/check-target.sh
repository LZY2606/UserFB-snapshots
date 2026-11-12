#!/bin/sh
set -eu
test -s test/mts-contract.cpp
build=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf "$build"; rm -f "$log"' EXIT
sh devcheck/configure.sh "$build"
cmake --build "$build" --parallel 4 --target ADLMIDI_static BankMapTest ConversionTest
c++ -std=c++11 -Iinclude test/mts-contract.cpp "$build/libADLMIDI.a" -o "$build/mts-contract"
if ! "$build/mts-contract" >"$log" 2>&1; then cat "$log"; exit 1; fi
cat "$log"
for marker in MTS_ACTIVE_OK MTS_PEDAL_OK MTS_VALIDATE_OK MTS_RESET_OK; do grep -qx "$marker" "$log"; done
ctest --test-dir "$build" --output-on-failure -R '^(BankMapTest|ConversionTest)$'
echo TARGET_MTS_OK
