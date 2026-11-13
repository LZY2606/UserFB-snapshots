#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$task_cache"' EXIT HUP INT TERM
mkdir -p "$task_cache/mod" "$task_cache/build" "$task_cache/path"
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOPROXY=off GOSUMDB=off
export GOMODCACHE="$task_cache/mod" GOCACHE="$task_cache/build" GOPATH="$task_cache/path"
export GOFLAGS=-mod=readonly
"$@"
