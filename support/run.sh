#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
TASK_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$TASK_CACHE"' EXIT HUP INT TERM
export GOCACHE="$TASK_CACHE/build" GOMODCACHE="$TASK_CACHE/modules"
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOENV=off GOWORK=off GOFLAGS=-mod=vendor CGO_ENABLED=0
case "${1:-}" in
baseline) go run ./support/baseline ;;
target) node support/target.cjs ;;
*) echo 'usage: sh support/run.sh baseline|target' >&2; exit 2 ;;
esac
