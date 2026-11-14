#!/bin/bash
set -euo pipefail
mkdir -p .task-build/cache .task-build/mod .task-build/path
export GOCACHE="$PWD/.task-build/cache" GOMODCACHE="$PWD/.task-build/mod" GOPATH="$PWD/.task-build/path"
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
exec go "$@"
