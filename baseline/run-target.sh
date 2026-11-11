#!/usr/bin/env bash
set -euo pipefail
test -s tests/stack_definite.rs
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
export CARGO_HOME="$PWD/baseline/.cargo-home" CARGO_TARGET_DIR="$b/build"
cargo test --locked --test stack_definite -- --nocapture | tee "$b/tap"
for name in byte_coverage joins_loops spill_alias verifier_hook; do grep -Eq "^test $name \.\.\. ok$" "$b/tap"; done
grep -Eq 'test result: ok\. [4-9][0-9]* passed; 0 failed; 0 ignored;' "$b/tap"
cargo test --locked --test ubpf_verifier
