#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s src/goo/entities/SceneResidency.js || { echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY src/goo/entities/SceneResidency.js" >&2; exit 1; }
exec node task_support/run.mjs
