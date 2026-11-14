#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
root=$(pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir -p .task-build/mutants
for name in residual_bias early_history; do
  mkdir "$scratch/$name"
  tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$scratch/$name"
  (cd "$scratch/$name"; patch -p1 < "task_support/defects/$name.patch") > ".task-build/mutants/$name.patch.log" 2>&1
  set +e
  (cd "$scratch/$name"; bash task_support/run.sh target) > ".task-build/mutants/$name.log" 2>&1
  status=$?
  set -e
  test "$status" -eq 4
  grep -Eq '^FAIL .*:[0-9]+ ' ".task-build/mutants/$name.log"
  echo "DETECTED_$name content_exit=$status"
done
echo MUTANTS_COMPLETE
