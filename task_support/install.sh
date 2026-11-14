#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v go >/dev/null
test -s vendor/modules.txt
ANT_TASK_CACHE=$(mktemp -d)
trap 'rm -rf "$ANT_TASK_CACHE"' EXIT
export GOMODCACHE="$ANT_TASK_CACHE/mod" GOCACHE="$ANT_TASK_CACHE/build" GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off
# This command only checks tools and pinned source; run.sh owns a fresh build cache.
go version
