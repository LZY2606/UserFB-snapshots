set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -I. -Isupport support/target_main.cpp -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
grep -Eq '^PASS layout/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS clock/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS continuation/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS failure/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
test "$(grep -Ec '^TARGET_OK cases=[1-9][0-9]*$' "$build/result")" = 1
bash support/baseline.sh
