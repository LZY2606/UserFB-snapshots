#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-target}" in
 baseline) node task_support/baseline.js ;;
 target) node task_support/runner.js ;;
 *) echo 'usage: bash task_support/run.sh baseline|target' >&2; exit 2 ;;
esac
