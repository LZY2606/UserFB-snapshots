#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if [[ -z ${TASK_GORTP_CACHE:-} ]]; then
 TASK_GORTP_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/gortp-go.XXXXXX")
 trap 'rm -rf "$TASK_GORTP_CACHE"' EXIT
fi
export GOMODCACHE="$TASK_GORTP_CACHE/mod" GOCACHE="$TASK_GORTP_CACHE/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=
mkdir -p "$GOMODCACHE" "$GOCACHE"
