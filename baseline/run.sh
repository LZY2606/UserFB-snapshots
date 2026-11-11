#!/bin/bash
set -euo pipefail
export CARGO_HOME="$PWD/.cargo-home"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
CARGO_TARGET_DIR="$build" cargo run --locked --manifest-path baseline/Cargo.toml --quiet
