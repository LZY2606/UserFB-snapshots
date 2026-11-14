#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash support/build.sh "$build/normal" target
"$build/normal/run" | tee "$build/normal/result.log"
for group in inverse serial stream; do
 grep -Eq "^GROUP $group checks=[1-9][0-9]*$" "$build/normal/result.log"
done
grep -Eq "^AMRWB_PULSE_OK checks=[1-9][0-9]*$" "$build/normal/result.log"
for mutation in sign2 track44; do
 v="$build/$mutation"
 mkdir -p "$v"
 cp wrapper.c enc_if.h "$v/"
 cp -R amrwbenc common support tests "$v/"
 (cd "$v" && git apply "support/mutations/$mutation.patch" && bash support/build.sh "$v/build" target)
 set +e
 "$v/build/run" > "$v/result.log" 2>&1
 status=$?
 set -e
 cat "$v/result.log"
 [[ $status == 42 ]] || { echo "NOT_CONTENT_FAILURE $mutation exit=$status"; exit 1; }
 grep -q '^ASSERT ' "$v/result.log"
 echo "CAUGHT $mutation"
done
bash support/baseline.sh
