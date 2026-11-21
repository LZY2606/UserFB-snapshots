#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if [[ -z ${TASK_LIVEGO_CACHE:-} ]]; then
 TASK_LIVEGO_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/livego-go.XXXXXX")
 trap 'rm -rf "$TASK_LIVEGO_CACHE"' EXIT
fi
export GOMODCACHE="$TASK_LIVEGO_CACHE/mod" GOCACHE="$TASK_LIVEGO_CACHE/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=vendor
mkdir -p "$GOMODCACHE" "$GOCACHE"
