#!/bin/bash
set -euo pipefail
export GOCACHE="$PWD/harness/.go-cache" GOTOOLCHAIN=local
cp harness/seed_test.go meltysynth/seed_test.go
trap 'rm -f meltysynth/seed_test.go' EXIT
pattern='^TestSeedAudio$'
if [[ ${1:-} == target ]]; then
  test -s meltysynth/modulator_routing_test.go
  pattern='^Test(SeedAudio|ModulatorRouting)$'
fi
out=$(go test -v ./meltysynth -run "$pattern" -count=1)
printf '%s\n' "$out"
grep -F -- '--- PASS: TestSeedAudio (' <<< "$out"
if [[ ${1:-} == target ]]; then grep -F -- '--- PASS: TestModulatorRouting (' <<< "$out"; fi
