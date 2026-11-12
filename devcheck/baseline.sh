#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
sh devcheck/configure.sh "$build"
cmake --build "$build" --parallel 4 --target ADLMIDI_static BankMapTest ConversionTest
c++ -std=c++11 -Iinclude devcheck/baseline.cpp "$build/libADLMIDI.a" -o "$build/baseline"
"$build/baseline"
ctest --test-dir "$build" --output-on-failure -R '^(BankMapTest|ConversionTest)$'
