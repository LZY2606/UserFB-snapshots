#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
for path in transparency_test.go examples/transparency/main.go; do
    if ! test -s "$path"; then
        echo "missing nonempty target file: $path" >&2
        exit 1
    fi
done
export GOTOOLCHAIN=local
export GOPATH="$PWD/.cache/gopath"
export GOCACHE="$PWD/.cache/build"
export GOMODCACHE="$PWD/.cache/mod"
mkdir -p .cache
events=$(mktemp "$PWD/.cache/transparency.XXXXXX")
trap 'rm -f "$events"' EXIT HUP INT TERM
if go test -mod=readonly -race -json -count=1 -timeout=45s -run '^(TestTransparency(Order|Alpha|Coverage|Lifecycle)|TestSeed(Opaque|OrderDependence|StraightAlpha|Crossing|Discard))$' . >"$events" 2>&1; then
    cat "$events"
else
    cat "$events"
    exit 1
fi
for name in TestTransparencyOrder TestTransparencyAlpha TestTransparencyCoverage TestTransparencyLifecycle TestSeedOpaque TestSeedOrderDependence TestSeedStraightAlpha TestSeedCrossing TestSeedDiscard; do
    if ! awk -v name="$name" 'index($0,"\"Action\":\"pass\"") && index($0,"\"Test\":\"" name "\"") {found=1} END {exit !found}' "$events"; then
        echo "missing passing test: $name" >&2
        exit 1
    fi
done
rm -f .cache/transparency.png
go run -mod=readonly ./examples/transparency
test -s .cache/transparency.png
echo TRANSPARENCY_DEMO_OK
