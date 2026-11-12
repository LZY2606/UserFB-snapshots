#!/usr/bin/env bash
set -euo pipefail
export CARGO_HOME="$PWD/.cargo-home"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
export CARGO_TARGET_DIR="$b"
test -s baseline/tests/vbri_seek.rs
cargo test --locked --manifest-path baseline/Cargo.toml --test vbri_seek -- --nocapture --test-threads=1 | tee "$b/new.log"
for name in vbri_layout indexed_seek reservoir_preroll fallback; do
 grep -Eq "^test ${name} \.\.\. .*ok$" "$b/new.log"
done
cargo test --locked --manifest-path baseline/Cargo.toml --test upstream -- --nocapture --test-threads=1 | tee "$b/old.log"
for name in test_is_maybe_info_tag_with_crc test_is_maybe_vbri_tag_with_crc; do
 grep -F "test demuxer::tests::$name ... ok" "$b/old.log"
done
cargo run --locked --quiet --manifest-path baseline/Cargo.toml --example start
printf 'VBRI_INDEX_OK cases=4\n'
