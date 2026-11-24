#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
build=$(mktemp -d "${TMPDIR:-/tmp}/wazero-numeric.XXXXXX")
trap 'rm -rf "$build"' EXIT
go test -buildvcs=false -count=1 -timeout 60s -v -run '^TestOriginStart' ./baseline
go test -buildvcs=false -count=1 -timeout 60s -v -run '^TestInterpreter_CallEngine_PushFrame$' ./internal/engine/interpreter | tee "$build/old-call.log"
grep -q -- '--- PASS: TestInterpreter_CallEngine_PushFrame ' "$build/old-call.log"
go test -buildvcs=false -count=1 -timeout 60s -v -run '^TestMemoryInstance_ReadUint32Le$' ./internal/wasm | tee "$build/old-memory.log"
grep -q -- '--- PASS: TestMemoryInstance_ReadUint32Le ' "$build/old-memory.log"
