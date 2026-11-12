set -eu
out=$1
grep -Eq '^CASE priority checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE assertions checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE shortest checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE completeness checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE isolation checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^TARGET_OK groups=5 checks=[1-9][0-9]*$' "$out"
