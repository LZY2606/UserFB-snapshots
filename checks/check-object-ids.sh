#!/bin/sh
# Regression check for the object-ID tracking pass.
# Builds in a temporary directory, runs the object-ID tests with the race
# detector, and renders a small color-coded PNG of the ownership map while
# printing the final pixel count per object ID.
set -eu
cd "$(dirname "$0")/.."

build=$(mktemp -d "${TMPDIR:-/tmp}/check-object-ids.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

png="$build/object-ids.png"

GOMODCACHE="$PWD/.gomodcache" \
GOCACHE="$build/cache" \
FAUXGL_OBJECT_ID_PNG="$png" \
go test -race -timeout 120s -count=1 -v \
    -run 'ObjectID|TestHorizontalRepro' .

cp "$png" object-ids.png
echo "object-ID map written to object-ids.png"
