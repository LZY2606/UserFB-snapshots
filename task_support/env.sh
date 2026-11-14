#!/usr/bin/env bash
set -euo pipefail
task_cache=$(mktemp -d /tmp/z2045-go.XXXXXX)
trap 'rm -rf "$task_cache"' EXIT
export GOMODCACHE="$task_cache/modules" GOCACHE="$task_cache/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
"$@"
