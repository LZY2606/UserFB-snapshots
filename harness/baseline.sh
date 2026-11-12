#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
GOWORK=off GOPROXY=off GOSUMDB=off GOMODCACHE="$build/gomod" go test -mod=readonly . -run '^TestCaptureBaseline$' -count=1 -v
