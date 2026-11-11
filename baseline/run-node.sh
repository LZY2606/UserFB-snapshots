#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
NODE_PATH="$PWD/baseline/node_modules" baseline/node_modules/.bin/esbuild "$1" --bundle --platform=node --format=cjs --outfile="$work/run.cjs"
node "$work/run.cjs"
