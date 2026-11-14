#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$work"' EXIT
source task_support/go-env.sh
go test ./pt -run '^TestSeedRGBTransport$' -v -count=1 > "$work/baseline.log" 2>&1
cat "$work/baseline.log"
grep -q '^--- PASS: TestSeedRGBTransport ' "$work/baseline.log"
grep -q 'OLD_RGB_TRANSPORT' "$work/baseline.log"
