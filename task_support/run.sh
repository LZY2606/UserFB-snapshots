#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
  baseline) node task_support/baseline.mjs ;;
  target)
    test -s test/style-update.test.mjs
    node task_support/verify.mjs
    node task_support/compatibility.mjs
    ;;
  *) exit 2 ;;
esac
