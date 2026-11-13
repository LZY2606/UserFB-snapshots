#!/usr/bin/env bash
set -euo pipefail
build=${CHECK_BUILD_DIR:-$(mktemp -d)}
if [[ -z "${CHECK_BUILD_DIR:-}" ]]; then trap 'rm -rf "$build"' EXIT; fi
cmake -S support -B "$build" -DCMAKE_BUILD_TYPE=Release -DCHECK_TARGET=OFF
cmake --build "$build" --target baseline -j 8
"$build/baseline"
if [[ "${1:-baseline}" == target ]]; then
 cmake -S support -B "$build" -DCMAKE_BUILD_TYPE=Release -DCHECK_TARGET=ON
 cmake --build "$build" --target target -j 8
 "$build/target" | tee "$build/target.log"
 for group in ellipse world persistence; do
  grep -Eq "^PASS $group/.+" "$build/target.log"
 done
 grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/target.log"
fi
