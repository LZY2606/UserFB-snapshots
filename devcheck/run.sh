#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export GOPROXY=off GOSUMDB=off
export GOCACHE="${GOCACHE:-$b/go-cache}"
case "${1:-baseline}" in
baseline)
 go run ./devcheck/cmd/baseline
 go test -v -count=1 -run '^Test(Reader(Simple|SingleStream|MultipleStreams)|CheckNone|WriterNoneCheck)$' . ;;
target)
 if go test -json -v -count=1 -run '^TestFilter(Delta|X86|ChainBlocks|Reject)$' . > "$b/tests" 2>&1; then
  cat "$b/tests"
 else cat "$b/tests"; exit 1; fi
 go run ./devcheck/cmd/check "$b/tests"
 bash devcheck/run.sh baseline ;;
*) exit 2;;
esac
