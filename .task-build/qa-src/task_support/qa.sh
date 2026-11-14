#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
export GOCACHE="$PWD/.task-build/gocache" GOPATH="$PWD/.task-build/gopath" GOMODCACHE="$PWD/.task-build/gopath/pkg/mod"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=readonly
go run ./task_support/qadriver .
