set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build" target
cp "$build/target" "$build/target"
"$build/target" > "$build/result" 2>&1 || { cat "$build/result"; exit 1; }
cat "$build/result"
grep -Eq '^PASS syndrome/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS sector_modes/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS hunk/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
grep -Eq '^PASS uncorrectable/[^ ]+ checks=[1-9][0-9]*$' "$build/result"
test "$(grep -Ec '^TARGET_OK cases=[1-9][0-9]*$' "$build/result")" = 1
bash support/baseline.sh
