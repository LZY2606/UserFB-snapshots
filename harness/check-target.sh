#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/ufb-shp-target-cache.XXXXXX")
trap 'rm -rf "$b"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$b/build" GOMODCACHE="$b/mod" GOPATH="$b/path"
go run harness/check.go
bash harness/baseline.sh
