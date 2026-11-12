#!/bin/sh
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
set -eu
log=$(mktemp)
cache=$(mktemp -d)
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$cache"
trap 'rm -f "$log"; rm -rf "$cache"' EXIT
if ! go test -count=1 -v -run '^TestPresentationSelectionContract$' ./mail >"$log" 2>&1; then cat "$log"; exit 1; fi
cat "$log"
grep -q '^--- PASS: TestPresentationSelectionContract ' "$log"
echo TARGET_PRESENTATION_OK
