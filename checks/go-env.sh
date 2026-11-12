#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local
export GOCACHE="$PWD/.cache/go-build"
export GOPATH="$PWD/.cache/gopath"
test "$(go env GOVERSION)" = go1.26.4 || { echo 'go1.26.4 required' >&2; exit 1; }
exec "$@"
