#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
mkdir -p .task-build
sandbox=$(mktemp -d)
trap 'rm -rf "$sandbox"' EXIT
for variant in origin restart; do
  tree="$sandbox/$variant"
  mkdir -p "$tree"
  tar --exclude=.task-build --exclude=.git -cf - . | tar -xf - -C "$tree"
  patch -d "$tree" -p1 < "task_support/variants/$variant.patch" > ".task-build/variant-$variant-patch.log" 2>&1
  set +e
  (cd "$tree" && bash task_support/run.sh target) > ".task-build/variant-$variant.log" 2>&1
  status=$?
  set -e
  [[ $status == 4 ]]
  grep -q '^FAIL ' ".task-build/variant-$variant.log"
  grep -q '^SEEK_PCM_DIFF ' ".task-build/variant-$variant.log"
  echo "VARIANT_CONTENT_REJECTED $variant status=$status"
done
