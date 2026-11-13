#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
export GOMODCACHE="$work/modules" GOCACHE="$work/build" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOENV=off GOWORK=off
mkdir -p "$GOMODCACHE" "$GOCACHE"
case "${1:-}" in
 baseline) go test -mod=vendor -tags baselinecheck -run '^Test' -v ./... ;;
 target) node support/target.cjs ;;
 *) echo 'usage: sh support/run.sh baseline|target' >&2; exit 2;;
esac
