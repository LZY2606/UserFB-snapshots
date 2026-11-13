set -euo pipefail
cd "$(dirname "$0")/.."
log=$(mktemp)
trap 'rm -f "$log"' EXIT
node --test --test-reporter=tap task_support/target.test.cjs > "$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
for group in mapping retiming repeat; do grep -Eq "^# TASK_GROUP $group checks=[1-9][0-9]* complete$" "$log"; done
grep -Eq '^# tests 3$' "$log"
grep -Eq '^# pass 3$' "$log"
grep -Eq '^# skipped 0$' "$log"
bash task_support/baseline.sh
printf 'TARGET_SUITE_COMPLETE\n'
