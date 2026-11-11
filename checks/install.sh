#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local
export GOPATH="$PWD/.cache/gopath"
export GOCACHE="$PWD/.cache/build"
export GOMODCACHE="$PWD/.cache/mod"
go version
go mod download all
go mod verify
go list -m all
