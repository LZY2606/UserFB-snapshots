#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
./node_modules/.bin/tsc --ignoreDeprecations 6.0 --outDir "$build"
CHECK_BUILD_DIR="$build" NODE_PATH="$PWD/node_modules" node regress/baseline.cjs
