#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -Isrc src/attribute.c src/char_ref.c src/error.c src/parser.c src/string_buffer.c src/string_piece.c src/tag.c src/tokenizer.c src/utf8.c src/util.c src/vector.c harness/baseline.c -o "$build/baseline"
"$build/baseline"
