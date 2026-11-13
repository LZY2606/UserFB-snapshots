#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
export GOCACHE="${TASK_RUN_CACHE:-${TMPDIR:-/tmp}/x1200-go-cache}"
case "${1:-baseline}" in
baseline)
  go test -mod=vendor -buildvcs=false -count=1 -v ./internal/crawler -run '^TestBaseline'
  go test -mod=vendor -buildvcs=false -count=1 ./...
  go build -mod=vendor -buildvcs=false -o "$GOCACHE/crawley" ./cmd/crawley
  echo BASELINE_OK
  ;;
target)
  test -f internal/links/css_refs_test.go
  test -f internal/crawler/css_imports_test.go
  go test -mod=vendor -buildvcs=false -count=1 -race -v ./internal/links ./internal/crawler -run '^Test(CSSImportTokens|CSSImportGraph|CSSImportCompatibility)$'
  go test -mod=vendor -buildvcs=false -count=1 ./...
  go build -mod=vendor -buildvcs=false -o "$GOCACHE/crawley" ./cmd/crawley
  echo CSS_IMPORTS_OK
  ;;
*) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2 ;;
esac
