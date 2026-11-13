#!/usr/bin/env bash
set -euo pipefail
for group in geometry scheduling scaling; do
  log="build/task/target-$group.log"
  set +e
  build/task/target "$group" >"$log" 2>&1
  rc=$?
  set -e
  cat "$log"
  [[ $rc -eq 0 ]] || exit "$rc"
  awk -v g="$group" '
    $1=="CASE_COMPLETE" && $2==g && $4 ~ /^checks=[1-9][0-9]*$/ { names[$3]=1; count++ }
    $1=="GROUP_COMPLETE" && $2==g && $3 ~ /^cases=[1-9][0-9]*$/ && $4 ~ /^checks=[1-9][0-9]*$/ { split($3,c,"="); split($4,k,"="); declared=c[2]; checks=k[2]; done++ }
    END { for(n in names) unique++; if(done!=1 || count<2 || unique!=count || declared!=count || checks<count) exit 1 }
  ' "$log"
done
printf '%s\n' TARGET_GROUPS_COMPLETE
