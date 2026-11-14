#!/bin/bash
# All third party packages come from the repository's tracked vendor tree.
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOWORK=off GOFLAGS=-mod=vendor
: "${TASK_SRT_CACHE_ROOT:=$(mktemp -d "${TMPDIR:-/tmp}/userfb-Z2004-go.XXXXXX")}"
export GOCACHE="$TASK_SRT_CACHE_ROOT/build" GOMODCACHE="$TASK_SRT_CACHE_ROOT/mod"
mkdir -p "$GOCACHE" "$GOMODCACHE"
