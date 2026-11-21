#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
cache_root=$(mktemp -d)
trap 'rm -rf "$cache_root"' EXIT
export CARGO_HOME="$cache_root/cargo" CARGO_TARGET_DIR="$PWD/.task-build/cargo" CARGO_NET_OFFLINE=true CARGO_INCREMENTAL=0
mkdir -p .task-build
manifest=harness/Cargo.toml
build_bin() {
  if ! cargo build --frozen --offline --manifest-path "$manifest" --bin "$1" >.task-build/build.log 2>&1; then
    tail -30 .task-build/build.log
    return 1
  fi
}
if [[ $mode == target ]]; then
  test -s src/mirrors.rs
  test -s harness/src/bin/mirror_demo.rs
  test -s harness/tests/mirror_transport.rs
  rm -rf "$CARGO_TARGET_DIR"
  build_bin api_binding
  find "$CARGO_TARGET_DIR/debug/deps" -name 'rpt-*.d' -type f -exec cat {} + > .task-build/rpt-dependencies.list
  grep -Fq '/src/mirrors.rs' .task-build/rpt-dependencies.list
  build_bin mirror_demo
  rm -f .task-build/mirrors.png
  .task-build/cargo/debug/mirror_demo .task-build/mirrors.png
  test -s .task-build/mirrors.png
  set +e
  cargo test --frozen --offline --manifest-path "$manifest" --test mirror_transport -- --nocapture --test-threads=1 >.task-build/target.log 2>&1
  target_code=$?
  set -e
  cat .task-build/target.log
  [[ $target_code == 0 ]] || exit "$target_code"
  grep -Eq 'test result: ok\. ([3-9]|[1-9][0-9]+) passed; 0 failed; 0 ignored;' .task-build/target.log
  for group in mirror_geometry mirror_visibility mirror_render; do
    grep -Eq "test ${group} \.\.\." .task-build/target.log
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" .task-build/target.log
  done
fi
build_bin baseline
.task-build/cargo/debug/baseline
