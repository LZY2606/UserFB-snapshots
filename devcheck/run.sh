#!/usr/bin/env bash
set -euo pipefail
entry=${1:-devcheck/baseline.mjs}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
node devcheck/tools/node_modules/esbuild/bin/esbuild "$entry" --bundle --platform=node --format=cjs --loader:.frag=text --loader:.vert=text --outfile="$work/run.cjs"
node --test "$work/run.cjs"
