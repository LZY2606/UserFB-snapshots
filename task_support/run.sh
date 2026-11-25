#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=baseline
if [[ $# -gt 0 ]]; then mode=$1; fi
if [[ $mode == baseline ]]; then exec node task_support/baseline.mjs; fi
[[ $mode == target ]]
test -s src/main/js/animation.js
test -s tests/animation.test.mjs
test -s examples/animate_frames.mjs
node --check src/main/js/animation.js
node task_support/animation_probe.mjs
out_log=$(mktemp)
out_json=$(mktemp)
trap 'rm -f "$out_log" "$out_json"' EXIT
set +e
node task_support/driver.mjs tests/animation.test.mjs >"$out_log" 2>&1
status=$?
set -e
cat "$out_log"
[[ $status == 0 ]] || exit "$status"
for group in clock cascade output; do
 grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$out_log"
done
grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$out_log"
rm -f "$out_json"
node examples/animate_frames.mjs task_support/example.ttml task_support/times.json "$out_json"
test -s "$out_json"
node task_support/example_check.mjs "$out_json"
node task_support/baseline.mjs
