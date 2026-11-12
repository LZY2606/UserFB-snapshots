set -eu
out=$1
grep -Eq '^CASE vectors checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE boundary checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE streaming checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE xof checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE lifecycle checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^TARGET_OK groups=5 checks=[1-9][0-9]*$' "$out"
