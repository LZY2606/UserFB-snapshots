#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cxx.XXXXXX")
trap 'rm -rf "$b"' EXIT
gc=-Wl,--gc-sections
if [ "$(uname -s)" = Darwin ]; then gc=-Wl,-dead_strip; fi
c++ -std=c++11 -O1 -ffunction-sections -fdata-sections -Wno-deprecated-register -Wno-deprecated-declarations -DPBRT_PROBES_NONE -Isrc -Isrc/core "$gc" checks/baseline.cpp src/core/geometry.cpp src/core/transform.cpp src/core/quaternion.cpp src/core/rng.cpp src/core/volume.cpp src/volumes/volumegrid.cpp checks/diagnostics.cpp -o "$b/baseline"
"$b/baseline"
