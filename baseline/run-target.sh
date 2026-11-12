#!/usr/bin/env bash
set -euo pipefail
export CARGO_HOME="$PWD/.cargo-home"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export CARGO_TARGET_DIR="$build"
test -s tests/msf_model.rs
test -s tools/check-msf-mutants.mjs
cargo test --locked --test msf_model -- --nocapture | tee "$build/target.log"
for name in model_layout relocation_law lazy_errors source_views; do
 grep -Eq "test ${name} \.\.\. ok" "$build/target.log"
done
cargo test --locked --lib | tee "$build/old.log"
grep -Eq 'test result: ok\. [1-9][0-9]* passed; 0 failed;' "$build/old.log"
node tools/check-msf-mutants.mjs
printf 'MSF_MODEL_OK\n'
