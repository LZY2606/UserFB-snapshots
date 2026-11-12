set -eu
out=$1
grep -Eq '^CASE interior checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE stale checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE shrink checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE ownership checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE callbacks checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^TARGET_OK groups=5 checks=[1-9][0-9]*$' "$out"
