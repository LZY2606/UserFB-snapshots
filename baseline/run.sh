#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
export CARGO_HOME="$PWD/baseline/.cargo-home" CARGO_TARGET_DIR="$b"
cargo test --locked --test baseline -- --nocapture
cargo test --locked --test ubpf_verifier
