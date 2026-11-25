#!/usr/bin/env bash
# Build the deliberate no-colored-transmission mutant and show that the target
# suite's transport assertion detects it.  A detected test failure is success
# for this check.
set -euo pipefail
cd "$(dirname "$0")/.."

cc=${CC:-cc}
mkdir -p .task-build/mutation
includes=(-I. -Isrc/core -Isrc/raytracer -Ilib/SimplexNoise -Ilib/cJSON)
flags=(-std=c11 -O2 -DUNBOUND_OBJECTS)

# Produce the normal objects and libscene.a first.
bash task_support/run.sh baseline >/dev/null

# Replace only lighttree.c with its mutation seam enabled.
"$cc" "${flags[@]}" "${includes[@]}" -DLIGHTTREE_DROP_COLORED_TRANSMISSION \
  -c src/raytracer/lighttree.c -o .task-build/mutation/lighttree_mut.o

"$cc" "${flags[@]}" "${includes[@]}" \
  task_support/driver.c tests/lighttree_test.c \
  .task-build/mutation/lighttree_mut.o .task-build/libscene.a \
  -lm -o .task-build/mutation/mutant_tests

set +e
output=$(.task-build/mutation/mutant_tests 2>&1)
status=$?
set -e
printf '%s\n' "$output"

if [[ $status -eq 0 ]]; then
  echo "MUTATION_NOT_DETECTED: dropping colored transmission passed all assertions" >&2
  exit 1
fi

if ! grep -Eq 'FAIL tests/lighttree_test\.c:[0-9]+ .*interval_matches_reference' <<<"$output"; then
  echo "MUTATION_WRONG_FAILURE: expected the transport interval assertion to fail" >&2
  exit 1
fi

echo "MUTATION_DETECTED: transport/colored_transmission rejects ignored transmission"
