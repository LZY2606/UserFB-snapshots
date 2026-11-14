#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export NODE_PATH="$PWD/task_support/deps"
export WS_NO_BUFFER_UTIL=1
export WS_NO_UTF_8_VALIDATE=1
exec node task_support/parent.js "${1:-target}"
