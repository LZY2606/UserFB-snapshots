#!/bin/sh
set -eu
export GOCACHE="$PWD/.build/go-cache"
export GOMODCACHE="$PWD/.build/modcache"
exec "$@"
