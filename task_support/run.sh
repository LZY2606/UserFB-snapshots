#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
node task_support/baseline.mjs
case "${1:-baseline}" in
baseline) ;;
target) test -s src/core/SceneRaycastIndex.js || { echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY src/core/SceneRaycastIndex.js" >&2; exit 1; }; test -s test/scene_raycast_index.test.mjs; node task_support/run.mjs ;;
*) exit 2 ;;
esac
