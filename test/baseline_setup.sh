#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cache_dir=$(mktemp -d)
trap 'rm -rf "$cache_dir"' EXIT
npm ci --cache "$cache_dir" --prefix test/baseline-tools --ignore-scripts --no-audit --no-fund
if [ ! -e node_modules ] && [ ! -L node_modules ]; then
  ln -s test/baseline-tools/node_modules node_modules
fi
node test/baseline_generate.mjs
