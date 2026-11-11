#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local
export GOPATH="$PWD/.cache/gopath"
export GOCACHE="$PWD/.cache/build"
export GOMODCACHE="$PWD/.cache/mod"
go test -mod=readonly -count=1 -timeout=15s -v -run '^TestSeed(Opaque|OrderDependence|StraightAlpha|Crossing|Discard)$' .
go test -mod=readonly ./cmd/...
mkdir -p .cache
go build -mod=readonly -o .cache/hello ./examples/hello.go
