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
  go test -mod=vendor ./browser ./jar -run '^Test(Redirect|CookieHeader|TabInheritance|LinksWithBaseTag|BrowserForm|SubmitMultipart|History)' -count=1 -v
  ;;
 target)
  test -s browser/frame_workflow_test.go || { echo 'missing browser/frame_workflow_test.go' >&2; exit 2; }
  go test -mod=vendor ./browser -run '^(TestFrameWorkflowRouting|TestFrameWorkflowLifetime|TestFrameWorkflowIsolation)$' -count=1 -v | tee "$scratch/target.log"
  grep -Fq -- "--- PASS: TestFrameWorkflowRouting (" "$scratch/target.log" || { echo "did not pass TestFrameWorkflowRouting" >&2; exit 3; }
  grep -Fq -- "--- PASS: TestFrameWorkflowLifetime (" "$scratch/target.log" || { echo "did not pass TestFrameWorkflowLifetime" >&2; exit 3; }
  grep -Fq -- "--- PASS: TestFrameWorkflowIsolation (" "$scratch/target.log" || { echo "did not pass TestFrameWorkflowIsolation" >&2; exit 3; }
  go test -mod=vendor ./browser ./jar -run '^Test(Redirect|CookieHeader|TabInheritance|LinksWithBaseTag|BrowserForm|SubmitMultipart|History)' -count=1 -v
  ;;
 *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2;;
esac
