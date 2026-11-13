#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
export GOCACHE="${TASK_RUN_CACHE:-${TMPDIR:-/tmp}/x1200-go-cache}"
go version
go list -mod=vendor ./... >/dev/null
echo OFFLINE_READY
