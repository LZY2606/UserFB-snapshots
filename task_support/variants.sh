#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/predictor_matrix.cpp
bash task_support/run.sh target
variant_root=$(mktemp -d)
trap 'rm -rf "$variant_root"' EXIT
for name in predictor matrix; do
  work="$variant_root/$name"
  mkdir -p "$work"
  cp -R codec task_support tests "$work/"
  (cd "$work" && patch -s -p1 < "task_support/variants/$name.patch")
  set +e
  (cd "$work" && bash task_support/run.sh target) > "$variant_root/$name.log" 2>&1
  status=$?
  set -e
  if [[ $status != 4 ]] || ! grep -q '^TARGET_LINK_OK$' "$variant_root/$name.log" || ! grep -q '^CONTENT_MISMATCH' "$variant_root/$name.log"; then
    tail -n 30 "$variant_root/$name.log"
    printf 'VARIANT_NOT_CAUGHT %s exit=%s\n' "$name" "$status"
    exit 1
  fi
  grep '^CONTENT_MISMATCH' "$variant_root/$name.log" | head -n 1
  printf 'VARIANT_CAUGHT %s exit=%s\n' "$name" "$status"
done
