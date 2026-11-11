#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=local
export GOCACHE="$PWD/.build/go-cache"
export GOMODCACHE="$PWD/.build/go-mod"
export GOPATH="$PWD/.build/go-path"
export GOFLAGS=-mod=readonly
go version
test "$(go env GOVERSION)" = go1.26.4
go mod download
go mod verify
