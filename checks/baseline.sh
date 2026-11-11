#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cp index.js "$b/"
cp -r lib "$b/lib"
mkdir "$b/checks"
cp checks/baseline.cjs "$b/checks/"
NODE_PATH="$PWD/checks/runtime/node_modules" node "$b/checks/baseline.cjs"
