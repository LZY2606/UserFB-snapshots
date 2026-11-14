#!/bin/bash
set -euo pipefail
mkdir -p .task-build/go-build .task-build/go-mod
export GOCACHE="$PWD/.task-build/go-build" GOMODCACHE="$PWD/.task-build/go-mod"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off CGO_ENABLED=0
# All production .go files of pe, including future additions, are compiled by Go.
go build -o .task-build/baseline ./task_support/baseline
