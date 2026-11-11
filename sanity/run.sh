#!/usr/bin/env bash
set -euo pipefail
export CARGO_HOME="${CARGO_HOME:-$PWD/.cargo-home}"
build=$(mktemp -d "${TMPDIR:-/tmp}/glyph-sanity.XXXXXX")
trap 'rm -rf "$build"' EXIT
export CARGO_TARGET_DIR="$build"
case "${1:-baseline}" in
baseline) cargo run --locked --offline --manifest-path sanity/Cargo.toml --quiet;;
soft_hyphens) cargo test --locked --offline --manifest-path sanity/Cargo.toml --test soft_hyphens -- --nocapture;;
*) exit 2;;
esac
