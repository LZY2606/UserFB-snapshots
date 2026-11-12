#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/expr-capsule.XXXXXX")
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
export GOCACHE="$build/cache" GOMODCACHE="$build/mod" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off
case "${1:?baseline or target}" in
 baseline)
  go run ./sanity/baseline
  go test -count=1 -v ./vm -run '^TestRun_(NilProgram|ReuseVM|ReuseVM_for_different_variables|TaggedFieldName)$'
  go test -count=1 -v ./optimizer -run '^TestOptimize_filter_map($|_)'
  ;;
 target) go run ./sanity/check ;;
 *) exit 2 ;;
esac
