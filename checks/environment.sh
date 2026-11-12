#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s pt/environment_target_test.go || { echo 'missing pt/environment_target_test.go' >&2; exit 1; }
log=$(mktemp)
trap 'rm -f "$log"' EXIT HUP INT TERM
sh checks/go-env.sh go test -json ./pt -run '^(TestEnvironmentDistribution|TestEnvironmentMIS|TestEnvironmentBranches)$' -count=1 > "$log"
cat "$log"
# Every named root must execute and pass; an empty match or skipped root fails.
for name in TestEnvironmentDistribution TestEnvironmentMIS TestEnvironmentBranches; do
 if ! grep -Eq '"Action":"pass".*"Test":"'"$name"'"' "$log"; then
  echo "target did not pass: $name" >&2; exit 1
 fi
done
sh checks/baseline.sh
sh checks/go-env.sh go run examples/environment/main.go
printf 'ENVIRONMENT_TARGET_OK groups=3\n'
