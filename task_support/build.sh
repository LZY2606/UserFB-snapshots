#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# Both caches start empty, with all network and toolchain downloads disabled.
rm -rf .task-build/gomodcache .task-build/gocache
mkdir -p .task-build/gomodcache .task-build/gocache
go build -o .task-build/baseline ./task_support/baseline
