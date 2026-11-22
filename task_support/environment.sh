#!/bin/bash
set -euo pipefail
mkdir -p .task-build
task_cache_root=$(mktemp -d "$PWD/.task-build/cache.XXXXXX")
mkdir "$task_cache_root/mod" "$task_cache_root/build" "$task_cache_root/path"
export GOENV=off GOTOOLCHAIN=local GOTELEMETRY=off GOWORK=off GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor
export GOMODCACHE="$task_cache_root/mod" GOCACHE="$task_cache_root/build" GOPATH="$task_cache_root/path"
