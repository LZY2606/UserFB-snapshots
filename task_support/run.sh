#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE="${GOCACHE:-$PWD/.cache/go-build}" GOMODCACHE="${GOMODCACHE:-$PWD/.cache/go-mod}"
mkdir -p .cache
case "${1:-}" in
 baseline)
  go test ./pt -run '^TestSeed(Geometry|Transport|CameraAndBounce)$' -v -count=1
  go build -o .cache/hello examples/hello.go
  echo 'PT_BASELINE_COMPLETE'
  ;;
 target)
  test -s pt/photon_target_test.go
  set +e
  go test ./pt -run '^TestPhoton(Transport|Gather|Progressive)$' -json -count=1 > .cache/target.jsonl 2>&1
  code=$?
  set -e
  cat .cache/target.jsonl
  test "$code" -eq 0
  go run task_support/verify.go TestPhotonTransport TestPhotonGather TestPhotonProgressive < .cache/target.jsonl
  bash task_support/run.sh baseline
  ;;
 *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;;
esac
