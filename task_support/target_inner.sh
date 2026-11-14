#!/bin/bash
set -euo pipefail
if [[ ! -s core/sourcemap_target_test.go ]]; then echo 'missing or empty target' >&2; exit 2; fi
mkdir -p .task-build
set +e
bash task_support/go.sh test -mod=vendor -count=1 -timeout=90s -v -run '^(TestSourceMapDiscovery|TestSourceMapProvenance|TestSourceMapGraph|TestSourceMapCompletion)$' ./core > .task-build/target.log 2>&1
status=$?
set -e
cat .task-build/target.log
if [[ $status -ne 0 ]]; then exit "$status"; fi
if grep -Eq -- '^--- SKIP:|^    --- SKIP:|^--- FAIL:|^    --- FAIL:' .task-build/target.log; then exit 4; fi
grep -Eq -- '^--- PASS: TestSourceMapDiscovery \(' .task-build/target.log || exit 5
grep -Eq -- '^--- PASS: TestSourceMapProvenance \(' .task-build/target.log || exit 5
grep -Eq -- '^--- PASS: TestSourceMapGraph \(' .task-build/target.log || exit 5
grep -Eq -- '^--- PASS: TestSourceMapCompletion \(' .task-build/target.log || exit 5
bash task_support/baseline.sh
