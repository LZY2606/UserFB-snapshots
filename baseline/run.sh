#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPATH="$PWD/.task-build/gopath" GOCACHE="$PWD/.task-build/go-cache" GOMODCACHE="$PWD/.task-build/modcache"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly
mkdir -p .task-build
go test -v -count=1 -skip '^(TestGraph|ExampleGraph)' ./...
printf '%s\n' JSONAPI_LEGACY_SUITE_OK
