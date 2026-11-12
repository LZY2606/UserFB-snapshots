set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -Iinclude -Isupport src/*.cpp support/target_main.cpp -o "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
grep -Eq '^PASS division/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS clock/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS lifecycle/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS compatibility/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
test "$(grep -Ec '^TARGET_OK cases=[1-9][0-9]*$' "$build/result")" = 1
bash support/baseline.sh
