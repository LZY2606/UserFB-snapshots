#!/bin/bash
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOFLAGS=-mod=readonly GOTELEMETRY=off
if [[ -z ${AUTHOR_Z2011_CACHE:-} ]]; then
  AUTHOR_Z2011_CACHE=$(mktemp -d "${TMPDIR:-/tmp}/author-z2011-cache.XXXXXX")
  export AUTHOR_Z2011_CACHE
  trap 'rm -rf "$AUTHOR_Z2011_CACHE"' EXIT
fi
export GOMODCACHE="$AUTHOR_Z2011_CACHE/mod" GOCACHE="$AUTHOR_Z2011_CACHE/build"
mkdir -p "$GOMODCACHE" "$GOCACHE"
