#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache=$(mktemp -d "${TMPDIR:-/tmp}/bz2-check.XXXXXX")
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS= CGO_ENABLED=0
export GOMODCACHE="$cache/modules" GOCACHE="$cache/build"
case "${1:-}" in
  baseline)
    go run -modfile=support/offline.mod -mod=readonly ./support/baseline
    go test -modfile=support/offline.mod -mod=readonly -count=1 -v ./bzip2
    printf '%s\n' BZ2_OLD_TESTS_OK
    ;;
  target)
    test -s bzip2/indexed_test.go
    go run -modfile=support/offline.mod -mod=readonly ./support/testguard
    go test -modfile=support/offline.mod -mod=readonly -count=1 ./bzip2
    printf '%s\n' BZ2_INDEX_AND_OLD_TESTS_OK
    ;;
  *) printf '%s\n' 'usage: bash support/check.sh baseline|target' >&2; exit 2 ;;
esac
