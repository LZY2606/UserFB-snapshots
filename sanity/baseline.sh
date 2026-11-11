#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || { d="${TMPDIR:-/tmp}/baseline_$$"; mkdir -p "$d"; printf '%s' "$d"; })
trap 'rm -rf "$build"' EXIT
extra=()
if [[ $(uname -s) == Darwin ]]; then extra=(-framework Accelerate); fi
c++ -std=c++11 -Isrc/Tonic sanity/baseline.cpp src/Tonic/{Generator,TableLookupOsc,TonicFrames,SampleTable,FixedValue,ControlGenerator,ControlValue,RampedValue,SineWave}.cpp "${extra[@]}" -o "$build/baseline"
"$build/baseline"
