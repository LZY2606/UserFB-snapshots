#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cp -R src checks package.json "$b/"
cd "$b"
node checks/baseline.mjs
