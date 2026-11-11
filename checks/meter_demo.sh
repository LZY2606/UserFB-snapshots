#!/usr/bin/env bash
# checks/meter_demo.sh -- build and run the MeterMap verification
# program, and re-run the pre-existing baseline checks.  Compiles in a
# temporary directory; no audio device is needed.  Prints METER_MAP_OK
# when every assertion has passed.
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/meter_demo.XXXXXX")
trap 'rm -rf "$build"' EXIT

# The old baseline must keep passing:
bash checks/baseline.sh

c++ -std=c++11 -Iinclude checks/meter_demo.cpp \
    src/MeterMap.cpp src/MidiFile.cpp src/MidiMessage.cpp \
    src/MidiEvent.cpp src/MidiEventList.cpp src/Binasc.cpp \
    -o "$build/meter_demo"
"$build/meter_demo"

echo "METER_MAP_OK"
