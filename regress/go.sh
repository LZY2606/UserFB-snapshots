#!/usr/bin/env bash
set -euo pipefail
export GOMODCACHE="$PWD/.build/mod" GOCACHE="$PWD/.build/cache" GOPATH="$PWD/.build/go"
exec go "$@"
