#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-go.XXXXXX")
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOMODCACHE="$b/mod" GOCACHE="$b/build"
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy
case "${1:-baseline}" in
 baseline) go run ./support/baseline;;
 target)
 go test -vet=off -json -v -count=1 -run '^TestUnit(Batch|Masks|Plan)$' . > "$b/target.json" || { cat "$b/target.json"; exit 1; }
 cat "$b/target.json"
 go run ./support/verify "$b/target.json" TestUnitBatch TestUnitMasks TestUnitPlan
 go test -vet=off -v -count=1 -run '^Test(NoParameterEvaluation|ParameterizedEvaluation|FunctionExecution)$' .
 ;;
 *) exit 2;;
esac
