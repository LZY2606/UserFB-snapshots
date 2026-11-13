set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_task task_support/target.cpp "$build/target" task_support/main.cpp
"$build/target" delay > "$build/delay.log"
cat "$build/delay.log"
grep -Eq "^TASK_GROUP delay checks=[1-9][0-9]* complete$" "$build/delay.log"
"$build/target" tank > "$build/tank.log"
cat "$build/tank.log"
grep -Eq "^TASK_GROUP tank checks=[1-9][0-9]* complete$" "$build/tank.log"
"$build/target" automation > "$build/automation.log"
cat "$build/automation.log"
grep -Eq "^TASK_GROUP automation checks=[1-9][0-9]* complete$" "$build/automation.log"
bash task_support/baseline.sh
printf 'TARGET_SUITE_COMPLETE\n'
