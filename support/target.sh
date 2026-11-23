#!/usr/bin/env bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOFLAGS=-mod=vendor
[[ -s chain_contract_test.go ]] || { echo 'missing chain_contract_test.go' >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
go build -o "$tmp/checkrun" ./support/checkrun
go test -json -count=1 -timeout=90s -run '^TestChain(PCM|Seek|Pages|Streaming)$' . > "$tmp/events.json" 2>&1 || { cat "$tmp/events.json"; exit 1; }
"$tmp/checkrun" < "$tmp/events.json"
go test -count=1 -timeout=90s -v -run '^(TestRead|TestFormat|TestLength|TestLengthForUnexpectedEof|TestSeek|TestEmpty)$' .
