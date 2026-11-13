set -euo pipefail
out=$1
for group in compat splices boundaries work; do
  test "$(grep -Ec "^CASE $group checks=[1-9][0-9]* PASS$" "$out")" -eq 1
done
test "$(grep -Ec '^EDIT_TARGET_OK groups=4 checks=[1-9][0-9]*$' "$out")" -eq 1
