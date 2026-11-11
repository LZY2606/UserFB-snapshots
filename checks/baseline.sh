#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cp -R src scripts test checks package.json rolldown.config.ts "$b/"
ln -s "$PWD/node_modules" "$b/node_modules"
cd "$b"
npm run trie:data
npm run trie:use
npm run trie:indic
npm run build
node checks/baseline.mjs
