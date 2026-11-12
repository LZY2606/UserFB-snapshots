#!/bin/bash
set -euo pipefail
source support/build.sh
c++ -std=c++17 "${INC[@]}" "$R/support/target_cases.cpp" "$R/support/driver.cpp" "$B/libap4.a" -o "$B/target"
"$B/target" > "$B/target.stdout"
cat "$B/target.stdout"
awk '
/^CASE (relocation|hierarchy|compat) [^ ]+ checks=[1-9][0-9]*$/ {n++; split($4,a,"="); checks+=a[2]}
/^GROUP (relocation|hierarchy|compat) cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {groups[$2]++; split($3,a,"="); gc+=a[2]; split($4,a,"="); gs+=a[2]}
/^TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {done++; split($2,a,"="); tc=a[2];split($3,a,"=");ts=a[2];last=NR}
END {if(groups["relocation"]!=1||groups["hierarchy"]!=1||groups["compat"]!=1||done!=1||n<3||gc!=n||tc!=n||checks!=gs||ts!=checks||last!=NR)exit 7}
' "$B/target.stdout"
CHECK_BUILD_DIR="$B" bash support/old_tests.sh
printf 'TARGET_AND_COMPAT_COMPLETE\n'
