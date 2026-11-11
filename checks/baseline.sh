#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -Iinclude checks/baseline.cpp src/MidiFile.cpp src/MidiMessage.cpp src/MidiEvent.cpp src/MidiEventList.cpp src/Binasc.cpp -o "$build/baseline"
"$build/baseline"
