#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for file in tests/chain_clock.cc tests/run_chain_clock.sh doc/chain_clock.md; do
  if [[ ! -s "$file" ]]; then printf 'MISSING_TARGET %s\n' "$file" >&2; exit 2; fi
done
bash devcheck/baseline.sh
build=$(mktemp -d "$PWD/.target.XXXXXX")
trap 'rm -rf "$build"' EXIT
bash devcheck/build-test.sh tests/chain_clock.cc "$build/chain-clock"
for style in queued streamed; do
  for rate in 44100 48000; do
    for ms in 20 60; do
      name="$style-$rate-$ms"
      "$build/chain-clock" --case "$name" > "$build/$name.log" 2>&1 || { cat "$build/$name.log"; exit 1; }
      cat "$build/$name.log"
      grep -qx "PASS $name chains=6" "$build/$name.log"
    done
  done
done
if "$build/chain-clock" --case does-not-exist > "$build/unknown.log" 2>&1; then
  echo 'ZERO_MATCH_ACCEPTED' >&2; exit 1
fi
bash tests/run_chain_clock.sh | tee "$build/mutations.log"
grep -qx 'DETECTED_CLOCK_FLOOR' "$build/mutations.log"
grep -qx 'DETECTED_WRONG_PREROLL' "$build/mutations.log"
printf '%s\n' 'CHAIN_CLOCK_TARGET_OK groups=8'
