#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-target}" in
 baseline) bash baseline/run-node.sh baseline/current.ts ;;
 target)
  test -s src/armature/skins/CoRSkinner.ts || { echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY src/armature/skins/CoRSkinner.ts" >&2; exit 1; }
  test -s test/cor-skinning.ts || { echo 'missing/empty target' >&2; exit 1; }
  log=$(mktemp)
  trap 'rm -f "$log"' EXIT
  if ! node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs task_support/runner.mjs >"$log" 2>&1; then cat "$log"; exit 1; fi
  cat "$log"
  for group in integrals deformation pipeline; do
   grep -Eq "^TASK_GROUP_OK $group checks=[1-9][0-9]*$" "$log" || exit 1
  done
  grep -qx TASK_TARGET_COMPLETE "$log"
  bash baseline/run-node.sh baseline/current.ts
  ;;
 *) echo 'usage: run.sh baseline|target' >&2;exit 2;;
esac
