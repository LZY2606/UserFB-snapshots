#!/usr/bin/env bash
set -euo pipefail
entry=${1:-devcheck/baseline.mjs}
work=$(mktemp -d "${TMPDIR:-/tmp}/devcheck.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
esbuild=devcheck/tools/node_modules/esbuild/bin/esbuild
# The installed bin may be a native executable or a node shim with a shebang;
# invoking it directly works for both.
"$esbuild" "$entry" --bundle --platform=node --format=cjs --loader:.frag=text --loader:.vert=text --outfile="$work/run.cjs"
node --test "$work/run.cjs"
