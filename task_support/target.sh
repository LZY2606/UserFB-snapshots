#!/usr/bin/env bash
set -euo pipefail
TASK_BUILD=$(mktemp -d); export TASK_BUILD
trap 'rm -rf "$TASK_BUILD"' EXIT
bash task_support/build.sh
c++ -std=c++17 -O1 -I. -Itask_support task_support/main.cpp task_tests/target.cpp "$TASK_BUILD/libschrift.a" -lm -o "$TASK_BUILD/target" > "$TASK_BUILD/build.log" 2>&1 || { cat "$TASK_BUILD/build.log"; exit 1; }
"$TASK_BUILD/target" --list > "$TASK_BUILD/list.log" 2>&1 || { cat "$TASK_BUILD/list.log"; exit 1; }
cat "$TASK_BUILD/list.log"
grep -qx TASK_LIST_COMPLETE "$TASK_BUILD/list.log"
"$TASK_BUILD/target" > "$TASK_BUILD/run.log" 2>&1 || { cat "$TASK_BUILD/run.log"; exit 1; }
cat "$TASK_BUILD/run.log"
# Verify completion outside the process: exit(0) inside a test cannot bypass it.
awk '
 FNR==NR {if($1=="REGISTER"){want[$2 SUBSEP $3]=1;total++}next}
 $1=="CASE" {k=$2 SUBSEP $3;if(!(k in want)||seen[k]++||$4!~/^checks=[1-9][0-9]*$/)bad=1;done++}
 END {if(bad||total==0||done!=total)exit 1;for(k in want)if(!seen[k])exit 1}
' "$TASK_BUILD/list.log" "$TASK_BUILD/run.log"
for group in parity geometry compatibility;do
 grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$TASK_BUILD/run.log"
done
test "$(grep -c '^TASK_SUITE_COMPLETE$' "$TASK_BUILD/run.log")" = 1
bash task_support/baseline.sh
