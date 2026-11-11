#!/bin/sh
# Build and run the maxiSyncOsc hard-sync regression.  Everything is
# compiled into a temporary directory; the working tree is not touched.
# Fails with a non-zero status if any numeric or behavioural check fails.
set -eu

b=$(mktemp -d "${TMPDIR:-/tmp}/hardsync.XXXXXX" 2>/dev/null) || {
    b="${TMPDIR:-/tmp}/hardsync.$$"
    mkdir -p "$b"
}
trap 'rm -rf "$b"' EXIT

root=$(cd "$(dirname "$0")/.." && pwd)

echo "== compiling standalone independent reference program =="
c++ -std=c++14 -O2 -I"$root/checks" \
    "$root/checks/hardsync_ref_main.cpp" \
    "$root/checks/hardsync_ref.cpp" \
    -o "$b/hardsync_ref"

echo "== compiling maxiSyncOsc regression =="
c++ -std=c++14 -O2 -Wall -Wextra \
    -I"$root/src" -I"$root/checks" \
    "$root/checks/hardsync.cpp" \
    "$root/src/libs/maxiSyncOsc.cpp" \
    "$root/src/maximilian.cpp" \
    "$root/checks/hardsync_ref.cpp" \
    -o "$b/hardsync"

echo "== running regression (also exercises the standalone program) =="
"$b/hardsync" "$b/hardsync_ref"
