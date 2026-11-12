#!/usr/bin/env bash
set -euo pipefail
test -s unicode_regression/unicode_test.go
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$cache/build" GOMODCACHE="$cache/modules"
go run -mod=vendor ./support/testguard '^TestUnicode(Coordinates|Scoring|Compatibility)$' TestUnicodeCoordinates TestUnicodeScoring TestUnicodeCompatibility
go run -mod=vendor ./support/repro
bash support/old.sh
