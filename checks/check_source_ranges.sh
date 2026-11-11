#!/bin/sh
# Builds the Node export of fontkit, generates the synthetic test font in a
# temporary directory, and verifies font.layoutWithSource source ranges.
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/check-source-ranges.XXXXXX")
trap 'rm -rf "$b"' EXIT
cp -R src scripts test checks package.json rolldown.config.ts "$b/"
ln -s "$PWD/node_modules" "$b/node_modules"
cd "$b"
npm run trie:data
npm run trie:use
npm run trie:indic
npm run build
node checks/make-source-test-font.mjs "$b/source-ranges-test.ttf"
node checks/check_source_ranges.mjs "$b/source-ranges-test.ttf"
