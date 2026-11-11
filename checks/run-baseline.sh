#!/bin/sh
set -eu
build=$(mktemp -d "${TMPDIR:-/tmp}/run-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
GOMODCACHE="$PWD/.gomodcache" GOCACHE="$build/cache" go test -timeout 10s -run '^TestBaseline$' -count=1 -v .
