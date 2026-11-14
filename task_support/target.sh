#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
run_groups() {
 local exe="$1" out="$2" rc=0
 for group in matrix history frames; do
   set +e
   "$exe" "$group" > "$out" 2>&1
   rc=$?
   set -e
   cat "$out"
   if [ "$rc" -ne 0 ]; then return "$rc"; fi
   grep -Eq "^DONE $group checks=[1-9][0-9]*$" "$out" || return 66
   grep -qx 'PROCESS_COMPLETE' "$out" || return 67
 done
}
run_groups ./.task-build/target .task-build/target.log
for change in task_support/mutations/*.patch; do
 temp=$(mktemp -d)
 git init -q "$temp"
 tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$temp"
 (cd "$temp" && git apply "$OLDPWD/$change" && bash task_support/build.sh)
 rc=0
 run_groups "$temp/.task-build/target" "$temp/result.log" || rc=$?
 if [ "$rc" -ne 42 ] || ! grep -q '^CONTENT_MISMATCH ' "$temp/result.log"; then
   echo "Mutation did not reach content assertion: $change exit=$rc" >&2
   rm -rf "$temp"; exit 68
 fi
 echo "DETECTED $change exit=$rc"
 rm -rf "$temp"
done
bash task_support/baseline.sh
