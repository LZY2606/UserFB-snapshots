#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
source task_support/env.sh
test ! -e task_baseline_test.go
cp task_support/baseline_test.go.in task_baseline_test.go
trap 'rm -f task_baseline_test.go' EXIT
go test -count=1 -v -timeout=30s "${production[@]}" utils_test.go task_baseline_test.go
