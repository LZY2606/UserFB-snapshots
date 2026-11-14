#!/bin/bash
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly GOWORK=off GOENV=off
export GOMODCACHE="$PWD/.task-build/gomod" GOCACHE="$PWD/.task-build/gocache"
export TMPDIR="$PWD/.task-build/tmp"
mkdir -p "$GOMODCACHE" "$GOCACHE" "$TMPDIR"
production=()
for source in *.go; do
  [[ $source == *_test.go ]] || production+=("$source")
done
