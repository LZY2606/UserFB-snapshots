#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$work"' EXIT
source task_support/go-env.sh
test -f pt/spectral_transport_test.go
go test -c -o "$work/target.test" ./pt
set +e
CHECK_TARGET=1 "$work/target.test" -test.run '^TestSpectral(Sensor|Media|Transport)$' -test.v -test.count=1 > "$work/target.log" 2>&1
status=$?
set -e
cat "$work/target.log"
if [ "$status" -ne 0 ]; then exit "$status"; fi
for name in TestSpectralSensor TestSpectralMedia TestSpectralTransport; do
  grep -q "^--- PASS: $name " "$work/target.log"
  grep -Eq "^[[:space:]]+--- PASS: $name/" "$work/target.log"
  grep -Eq "^GUARD $name [1-9][0-9]*$" "$work/target.log"
done
grep -qx GUARD_COMPLETE "$work/target.log"
"$work/target.test" -test.run '^TestSeedRGBTransport$' -test.v -test.count=1 > "$work/baseline.log" 2>&1
cat "$work/baseline.log"
grep -q '^--- PASS: TestSeedRGBTransport ' "$work/baseline.log"
