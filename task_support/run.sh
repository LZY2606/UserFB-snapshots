#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
scratch=$(mktemp -d "${TMPDIR:-/tmp}/task.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
export GOCACHE="$scratch/gocache" GOTMPDIR="$scratch"
case "${1:-}" in
 baseline)
  go run -mod=vendor ./task_support/baseline
  go test -mod=vendor ./pkg/compiler ./pkg/runtime/expressions ./pkg/runtime/expressions/clauses -run '^Test(For|Limit|Sort|Collect|Body|Return|Program)' -count=1 -v
  ;;
 target)
  test -s collection_stream_test.go || { echo 'missing collection_stream_test.go' >&2; exit 2; }
  go test -mod=vendor . -run '^(TestCollectionStreamShape|TestCollectionStreamDemand|TestCollectionStreamIsolation)$' -count=1 -v | tee "$scratch/target.log"
  grep -Fq -- "--- PASS: TestCollectionStreamShape (" "$scratch/target.log" || { echo "did not pass TestCollectionStreamShape" >&2; exit 3; }
  grep -Fq -- "--- PASS: TestCollectionStreamDemand (" "$scratch/target.log" || { echo "did not pass TestCollectionStreamDemand" >&2; exit 3; }
  grep -Fq -- "--- PASS: TestCollectionStreamIsolation (" "$scratch/target.log" || { echo "did not pass TestCollectionStreamIsolation" >&2; exit 3; }
  go test -mod=vendor ./pkg/compiler ./pkg/runtime/expressions ./pkg/runtime/expressions/clauses -run '^Test(For|Limit|Sort|Collect|Body|Return|Program)' -count=1 -v
  ;;
 *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;;
esac
