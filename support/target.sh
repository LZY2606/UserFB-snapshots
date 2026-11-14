set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh support/target_main.cpp "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
grep -Eq '^PASS tetrahedra/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS pipeline/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS alpha/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS ownership/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
test "$(grep -Ec '^TARGET_OK cases=[1-9][0-9]*$' "$build/result")" = 1
bash support/baseline.sh
