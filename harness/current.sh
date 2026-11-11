#!/usr/bin/env bash
set -euo pipefail
export CARGO_HOME="$PWD/.cargo-home"
export TMPDIR="${TMPDIR:-/tmp}"
build=$(mktemp -d -p "$TMPDIR")
trap 'rm -rf "$build"' EXIT
cargo run --locked --manifest-path harness/Cargo.toml --target-dir "$build" --bin current
mkdir -p harness/tests/images/gradients
for fixture in well-behaved-radial conical-smaller-radial simple-radial-with-ts-hq; do
  cp "tests/images/gradients/$fixture.png" "harness/tests/images/gradients/$fixture.png"
done
for name in well_behaved_radial conical_smaller_radial simple_radial_with_ts_hq; do
  out=$(cargo test --locked --manifest-path harness/Cargo.toml --target-dir "$build" --test old_radial "gradients::$name" -- --exact --nocapture)
  printf '%s\n' "$out"
  grep -F "test gradients::$name ... ok" <<< "$out"
done
printf '%s\n' TINY_SKIA_OLD_RADIAL_OK
