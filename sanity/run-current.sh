#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d); trap 'rm -rf "$build"' EXIT
./node_modules/.bin/tsc --incremental false --outDir "$build"
./node_modules/.bin/peggy --cache --format es --allowed-start-rules program,constant_expression -o "$build/src/preprocessor/preprocessor-parser.js" src/preprocessor/preprocessor-grammar.pegjs
./node_modules/.bin/peggy --cache --format es -o "$build/src/parser/parser.js" src/parser/glsl-grammar.pegjs
printf '{"type":"module"}\n' > "$build/package.json"
cp sanity/current.mjs "$build/current.mjs"
node "$build/current.mjs" "${1:-}"
