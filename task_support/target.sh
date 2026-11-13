set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/qmdsp-target.XXXXXX")
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_task task_support/target.cpp "$build/target" task_support/main.cpp
"$build/target" adjoint > "$build/adjoint.log"
cat "$build/adjoint.log"
grep -Eq "^TASK_GROUP adjoint checks=[1-9][0-9]* complete$" "$build/adjoint.log"
"$build/target" solve > "$build/solve.log"
cat "$build/solve.log"
grep -Eq "^TASK_GROUP solve checks=[1-9][0-9]* complete$" "$build/solve.log"
"$build/target" editing > "$build/editing.log"
cat "$build/editing.log"
grep -Eq "^TASK_GROUP editing checks=[1-9][0-9]* complete$" "$build/editing.log"
bash task_support/baseline.sh
printf 'TARGET_SUITE_COMPLETE\n'
