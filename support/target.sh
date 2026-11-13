#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .cache
b=$(mktemp -d .cache/target.XXXXXX)
trap 'rm -rf "$b"' EXIT
test -s checks/differentials_test.cpp
bash support/build.sh support/driver.cpp checks/differentials_test.cpp -o "$b/contract"
set +e
"$b/contract" | tee .cache/target.stdout
statuses=("${PIPESTATUS[@]}")
set -e
if (( statuses[0] != 0 )); then exit "${statuses[0]}"; fi
if (( statuses[1] != 0 )); then exit "${statuses[1]}"; fi
awk -f support/verify_output.awk .cache/target.stdout
bash support/baseline.sh
