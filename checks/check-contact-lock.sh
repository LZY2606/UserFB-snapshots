#!/bin/sh
# Builds and runs the windowless foot_ik contact-track locking regression.
#
# Only the ozz core libraries and the regression executable are built: no FBX
# SDK, GLFW windowing or large media asset is required. The skeleton, contact
# tracks, root trajectory and ground are all generated programmatically.
#
# An optional first argument selects the debug CSV output path (defaults to
# contact_lock_regression.csv in the current directory).
set -eu

csv=${1:-contact_lock_regression.csv}

build=$(mktemp -d "${TMPDIR:-/tmp}/contact-lock.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

cmake -S checks -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target contact_lock_regression -j 2
"$build/contact_lock_regression" "$csv"
