#!/bin/bash
set -euo pipefail
export CARGO_HOME="$PWD/.cargo-home"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
CARGO_TARGET_DIR="$build" cargo test --locked --lib pe::exception::tests -- --nocapture 2>&1 | tee "$build/old.log"
grep -Eq 'test result: ok\. [1-9][0-9]* passed' "$build/old.log"
echo OLD_EXCEPTION_TESTS_OK
