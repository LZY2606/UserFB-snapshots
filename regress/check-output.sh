set -eu
out=$1
grep -Eq '^CASE sharing checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE words checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE branches checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE storage checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^CASE reuse checks=[1-9][0-9]* PASS$' "$out"
grep -Eq '^TARGET_OK groups=5 checks=[1-9][0-9]*$' "$out"
