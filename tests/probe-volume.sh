#!/bin/sh
#
# tests/probe-volume.sh
#
# Builds the ProbeVolume module together with the existing SH/ZH libraries,
# runs the numeric assertions, and generates tests/out/probes.ppm.
# Also re-runs the existing probe-baseline convention checks.
#
set -e

cd "$(dirname "$0")/.."

mkdir -p tests/out

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:-"-std=c++11 -O2 -Wall"}

echo "== building probe-volume tests =="
$CXX $CXXFLAGS -I. SHLib.cpp ZHLib.cpp ProbeVolume.cpp \
    tests/probe-volume-test.cpp -o tests/out/probe-volume-test

echo "== running probe-volume numeric assertions =="
./tests/out/probe-volume-test

echo "== building/running baseline convention checks =="
$CXX -std=c++11 -O2 -I. SHLib.cpp ZHLib.cpp \
    tests/probe-baseline.cpp -o tests/out/probe-baseline
./tests/out/probe-baseline

echo "== checking generated image =="
test -s tests/out/probes.ppm
magic=$(head -c 2 tests/out/probes.ppm)
if [ "$magic" != "P6" ]; then
    echo "probes.ppm is not a binary PPM file" >&2
    exit 1
fi

echo "== probe-volume checks complete; image at tests/out/probes.ppm =="
