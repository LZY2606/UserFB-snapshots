#!/usr/bin/env bash
# Faithful local re-creation of task_support/target.sh's mutation loop,
# replacing only `mktemp -d`/`git init` (blocked by this Seatbelt sandbox)
# with repo-local directories.  Predicates are identical.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
run_groups() {
 local exe="$1" out="$2" rc=0
 for group in codes runs packets; do
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
rm -rf .task-build/sim && mkdir -p .task-build/sim
for change in task_support/mutations/*.patch; do
 temp="$ROOT/.task-build/sim/$(basename "$change").d"
 mkdir -p "$temp"
 tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$temp"
 ( cd "$temp" && git apply "$ROOT/$change" && bash task_support/build.sh )
 rc=0
 run_groups "$temp/.task-build/target" "$temp/result.log" || rc=$?
 if [ "$rc" -ne 42 ] || ! grep -q '^CONTENT_MISMATCH ' "$temp/result.log"; then
   echo "Mutation did not reach content assertion: $change exit=$rc" >&2
   exit 68
 fi
 echo "DETECTED $change exit=$rc"
 rm -rf "$temp"
done
echo "SIM_TARGET_OK"
