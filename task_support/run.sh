#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE="${TMPDIR:-/tmp}/gocache"
case "${1:-baseline}" in
 baseline)
  go test -mod=vendor -count=1 -timeout=45s -v -run '^TestTaskBaseline' .
  go test -mod=vendor -count=1 -timeout=45s ./pkg/playlist/...
  ;;
 target)
  test -f task_hls_aes_test.go
  test -f pkg/playlist/task_aes_map_test.go
  go test -mod=vendor -count=1 -timeout=60s -v -run '^(TestHLSAES|TestTaskBaseline)' . ./pkg/playlist/...
  ;;
 *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;;
esac
