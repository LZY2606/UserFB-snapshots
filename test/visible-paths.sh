#!/bin/sh
# Runs the analytical visibility clipping test group (TestVisiblePathsTarget),
# proves that the group actually executed, and checks that the SVG showing the
# middle occlusion gap was produced.
set -u

cd "$(dirname "$0")/.."

mkdir -p test/out

log=$(go test ./ln -run '^TestVisiblePathsTarget$' -v -count=1 2>&1)
status=$?
printf '%s\n' "$log"

if ! printf '%s\n' "$log" | grep -q '^=== RUN   TestVisiblePathsTarget$'; then
	echo "visible-paths: TestVisiblePathsTarget did not run" >&2
	exit 1
fi
if [ "$status" -ne 0 ] || ! printf '%s\n' "$log" | grep -q '^--- PASS: TestVisiblePathsTarget'; then
	echo "visible-paths: TestVisiblePathsTarget did not pass" >&2
	exit 1
fi

svg=test/out/visibility.svg
if [ ! -f "$svg" ]; then
	echo "visible-paths: $svg was not written" >&2
	exit 1
fi

# The occlusion gap must be visible: both the full and the zoomed row are
# split, so the file contains at least four polylines.
n=$(grep -c '<polyline' "$svg")
if [ "$n" -lt 4 ]; then
	echo "visible-paths: expected at least 4 polylines in $svg, got $n" >&2
	exit 1
fi

echo "visible-paths: OK ($n polylines, gap visible in $svg)"
