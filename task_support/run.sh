#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export npm_config_cache="$PWD/.task-build/npm-cache"
export npm_config_offline=true
exec node task_support/run.cjs "${1:-baseline}"
