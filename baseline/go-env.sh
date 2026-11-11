#!/bin/sh
set -eu
export GOTOOLCHAIN=local
export GOTELEMETRY=off
export GOENV=off
export GOCACHE="$PWD/.build/go-cache"
export GOMODCACHE="$PWD/.build/modcache"
export GOTMPDIR="$PWD/.build/tmp"
export TMPDIR="$PWD/.build/tmp"
mkdir -p "$GOCACHE" "$GOMODCACHE" "$GOTMPDIR"
exec "$@"
