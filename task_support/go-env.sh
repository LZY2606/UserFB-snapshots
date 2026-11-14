#!/bin/bash
# Source this from a support runner after assigning work to a new temporary directory.
export GO111MODULE=off GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local CGO_ENABLED=0
export GOPATH="$work/gopath" GOCACHE="$work/cache" GOMODCACHE="$work/modcache"
mkdir -p "$GOPATH" "$GOCACHE" "$GOMODCACHE"
