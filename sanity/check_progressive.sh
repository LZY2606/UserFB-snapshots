#!/usr/bin/env bash
# Build and run the progressive-mesh sanity scenarios in a temporary
# directory, using the Eigen checkout locked under .deps (see install.sh).
set -euo pipefail

# Locate the repository root from this script's location so the run starting
# point does not depend on the caller's cwd.
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if [ ! -s .deps/eigen/Eigen/Core ]; then
  echo "missing .deps/eigen (run sanity/install.sh)" >&2
  exit 1
fi
if [ ! -s sanity/progressive.cpp ]; then
  echo "missing sanity/progressive.cpp" >&2
  exit 1
fi

b=$(mktemp -d "${TMPDIR:-/tmp}/progressive.XXXXXX")
trap 'rm -rf "$b"' EXIT

c++ -std=c++17 -O0 -I include -I .deps/eigen \
  sanity/progressive.cpp -pthread -o "$b/progressive"

# The binary itself runs a non-zero number of scenarios and exits non-zero on
# any failed assertion, missing data, or zero work.
out=$("$b/progressive")
echo "$out"

# Every required scenario line must appear, in order.
want=(
  "PASS full-roundtrip"
  "PASS independent-components"
  "PASS selective-connected"
  "PASS attributes"
  "LIBIGL_PROGRESSIVE_OK"
)
for line in "${want[@]}"; do
  if ! grep -Fxq "$line" <<<"$out"; then
    echo "missing expected output line: $line" >&2
    exit 1
  fi
done

# Count the scenarios that actually ran.
nscen=$(grep -c '^PASS ' <<<"$out")
if [ "$nscen" -eq 0 ]; then
  echo "zero scenarios executed" >&2
  exit 1
fi
