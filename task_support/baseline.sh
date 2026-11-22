#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOTOOLCHAIN=local
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export GOMODCACHE=$cache_root/mod GOCACHE=$cache_root/build
go test -mod=vendor -count=1 -v -run '^TestBaseline$' .
