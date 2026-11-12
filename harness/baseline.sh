#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-shp-cache.XXXXXX")
trap 'rm -rf "$b"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$b/build" GOMODCACHE="$b/mod" GOPATH="$b/path"
go test -vet=off -count=1 -v -run '^(TestReadPoint|TestSequentialReader|TestZipReader)$' .
echo BASELINE_SHP_LOCAL_SEQUENTIAL_ZIP_PASS
